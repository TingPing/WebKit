/*
 * Copyright (C) 2026 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebAuthenticatorCoordinatorProxy.h"

#if ENABLE(WEB_AUTHN)

#include "WebAuthenticationRequestData.h"
#include <WebCore/AuthenticatorAttachment.h>
#include <WebCore/AuthenticatorResponseData.h>
#include <WebCore/CredentialPropertiesOutput.h>
#include <WebCore/ExceptionData.h>
#include <WebCore/SecurityOriginData.h>
#include <WebCore/WebAuthenticationUtils.h>
#include <gio/gio.h>
#include <wtf/JSONValues.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/glib/GRefPtr.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/text/Base64.h>
#include <wtf/text/MakeString.h>

namespace WebKit {
using namespace WebCore;

// The gateway interface of credentialsd, normally called by the Credential
// portal of xdg-desktop-portal. Calling it directly requires the daemon to
// trust the caller, which its development builds control with the
// CREDSD_TRUSTED_CALLERS and CREDSD_TRUSTED_APP_IDS environment variables.
static const char credentialsServiceName[] = "xyz.iinuwa.credentialsd.Credentials";
static const char credentialsObjectPath[] = "/org/freedesktop/portal/desktop";
static const char credentialsInterfaceName[] = "org.freedesktop.handler.portal.experimental.Credential";
static const int credentialsCallTimeout = 300000;

static const char* webAuthnApplicationID()
{
    static GUniquePtr<char> applicationID = [] {
        const char* environmentID = g_getenv("WEBKIT_WEBAUTHN_APP_ID");
        if (environmentID && *environmentID)
            return GUniquePtr<char>(g_strdup(environmentID));
        if (auto* application = g_application_get_default()) {
            if (const char* identifier = g_application_get_application_id(application))
                return GUniquePtr<char>(g_strdup(identifier));
        }
        return GUniquePtr<char>(g_strdup("org.webkit.WebKitApplication"));
    }();
    return applicationID.get();
}

static ASCIILiteral userVerificationRequirementString(UserVerificationRequirement requirement)
{
    switch (requirement) {
    case UserVerificationRequirement::Required:
        return "required"_s;
    case UserVerificationRequirement::Preferred:
        return "preferred"_s;
    case UserVerificationRequirement::Discouraged:
        return "discouraged"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral residentKeyRequirementString(ResidentKeyRequirement requirement)
{
    switch (requirement) {
    case ResidentKeyRequirement::Required:
        return "required"_s;
    case ResidentKeyRequirement::Preferred:
        return "preferred"_s;
    case ResidentKeyRequirement::Discouraged:
        return "discouraged"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral attestationConveyancePreferenceString(AttestationConveyancePreference preference)
{
    switch (preference) {
    case AttestationConveyancePreference::None:
        return "none"_s;
    case AttestationConveyancePreference::Indirect:
        return "indirect"_s;
    case AttestationConveyancePreference::Direct:
        return "direct"_s;
    case AttestationConveyancePreference::Enterprise:
        return "enterprise"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral authenticatorAttachmentString(AuthenticatorAttachment attachment)
{
    switch (attachment) {
    case AuthenticatorAttachment::Platform:
        return "platform"_s;
    case AuthenticatorAttachment::CrossPlatform:
        return "cross-platform"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<AuthenticatorTransport> authenticatorTransportFromString(const String& transport)
{
    if (transport == "usb"_s)
        return AuthenticatorTransport::Usb;
    if (transport == "nfc"_s)
        return AuthenticatorTransport::Nfc;
    if (transport == "ble"_s)
        return AuthenticatorTransport::Ble;
    if (transport == "internal"_s)
        return AuthenticatorTransport::Internal;
    if (transport == "hybrid"_s)
        return AuthenticatorTransport::Hybrid;
    if (transport == "smart-card"_s)
        return AuthenticatorTransport::SmartCard;
    return std::nullopt;
}

static Ref<JSON::Object> credentialDescriptorToJSON(const PublicKeyCredentialDescriptor& descriptor)
{
    auto object = JSON::Object::create();
    object->setString("type"_s, "public-key"_s);
    object->setString("id"_s, base64URLEncodeToString(descriptor.id.span()));
    if (!descriptor.transports.isEmpty()) {
        auto transports = JSON::Array::create();
        for (auto transport : descriptor.transports)
            transports->pushString(toString(transport));
        object->setArray("transports"_s, WTF::move(transports));
    }
    return object;
}

static RefPtr<JSON::Object> extensionInputsToJSON(const std::optional<AuthenticationExtensionsClientInputs>& extensions, bool isGetAssertion)
{
    if (!extensions)
        return nullptr;

    auto object = JSON::Object::create();
    bool hasExtensions = false;
    if (isGetAssertion && !extensions->appid.isNull()) {
        object->setString("appid"_s, extensions->appid);
        hasExtensions = true;
    }
    if (!isGetAssertion && extensions->credProps) {
        object->setBoolean("credProps"_s, *extensions->credProps);
        hasExtensions = true;
    }
    if (extensions->largeBlob) {
        auto largeBlob = JSON::Object::create();
        if (!extensions->largeBlob->support.isNull())
            largeBlob->setString("support"_s, extensions->largeBlob->support);
        if (extensions->largeBlob->read)
            largeBlob->setBoolean("read"_s, *extensions->largeBlob->read);
        if (extensions->largeBlob->write)
            largeBlob->setString("write"_s, base64URLEncodeToString(extensions->largeBlob->write->span()));
        object->setObject("largeBlob"_s, WTF::move(largeBlob));
        hasExtensions = true;
    }
    if (!hasExtensions)
        return nullptr;
    return object;
}

static String serializeMakeCredentialOptions(const PublicKeyCredentialCreationOptions& options)
{
    auto object = JSON::Object::create();

    auto rp = JSON::Object::create();
    rp->setString("name"_s, options.rp.name);
    if (!options.rp.id.isNull())
        rp->setString("id"_s, options.rp.id);
    object->setObject("rp"_s, WTF::move(rp));

    auto user = JSON::Object::create();
    user->setString("id"_s, base64URLEncodeToString(options.user.id.span()));
    user->setString("name"_s, options.user.name);
    user->setString("displayName"_s, options.user.displayName);
    object->setObject("user"_s, WTF::move(user));

    object->setString("challenge"_s, base64URLEncodeToString(options.challenge.span()));

    auto parameters = JSON::Array::create();
    for (const auto& parameter : options.pubKeyCredParams) {
        auto parameterObject = JSON::Object::create();
        parameterObject->setString("type"_s, "public-key"_s);
        parameterObject->setInteger("alg"_s, static_cast<int>(parameter.alg));
        parameters->pushObject(WTF::move(parameterObject));
    }
    object->setArray("pubKeyCredParams"_s, WTF::move(parameters));

    if (options.timeout)
        object->setInteger("timeout"_s, static_cast<int>(*options.timeout));

    if (!options.excludeCredentials.isEmpty()) {
        auto excludeCredentials = JSON::Array::create();
        for (const auto& descriptor : options.excludeCredentials)
            excludeCredentials->pushObject(credentialDescriptorToJSON(descriptor));
        object->setArray("excludeCredentials"_s, WTF::move(excludeCredentials));
    }

    if (options.authenticatorSelection) {
        auto selection = JSON::Object::create();
        if (options.authenticatorSelection->authenticatorAttachment)
            selection->setString("authenticatorAttachment"_s, authenticatorAttachmentString(*options.authenticatorSelection->authenticatorAttachment));
        if (options.authenticatorSelection->residentKey)
            selection->setString("residentKey"_s, residentKeyRequirementString(*options.authenticatorSelection->residentKey));
        selection->setBoolean("requireResidentKey"_s, options.authenticatorSelection->requireResidentKey);
        selection->setString("userVerification"_s, userVerificationRequirementString(options.authenticatorSelection->userVerification));
        object->setObject("authenticatorSelection"_s, WTF::move(selection));
    }

    object->setString("attestation"_s, attestationConveyancePreferenceString(options.attestation));

    if (auto extensions = extensionInputsToJSON(options.extensions, false))
        object->setObject("extensions"_s, extensions.releaseNonNull());

    return object->toJSONString();
}

static String serializeGetAssertionOptions(const PublicKeyCredentialRequestOptions& options)
{
    auto object = JSON::Object::create();

    object->setString("challenge"_s, base64URLEncodeToString(options.challenge.span()));

    if (options.timeout)
        object->setInteger("timeout"_s, static_cast<int>(*options.timeout));

    if (!options.rpId.isNull())
        object->setString("rpId"_s, options.rpId);

    if (!options.allowCredentials.isEmpty()) {
        auto allowCredentials = JSON::Array::create();
        for (const auto& descriptor : options.allowCredentials)
            allowCredentials->pushObject(credentialDescriptorToJSON(descriptor));
        object->setArray("allowCredentials"_s, WTF::move(allowCredentials));
    }

    object->setString("userVerification"_s, userVerificationRequirementString(options.userVerification));

    if (auto extensions = extensionInputsToJSON(options.extensions, true))
        object->setObject("extensions"_s, extensions.releaseNonNull());

    return object->toJSONString();
}

static RefPtr<ArrayBuffer> arrayBufferFromBase64URLField(const JSON::Object& object, ASCIILiteral key)
{
    auto value = object.getString(key);
    if (!value)
        return nullptr;
    auto decoded = base64URLDecode(value);
    if (!decoded)
        return nullptr;
    return ArrayBuffer::create(*decoded);
}

static std::optional<AuthenticationExtensionsClientOutputs> extensionOutputsFromJSON(const JSON::Object& object)
{
    AuthenticationExtensionsClientOutputs outputs;
    bool hasOutputs = false;
    if (auto appid = object.getBoolean("appid"_s)) {
        outputs.appid = *appid;
        hasOutputs = true;
    }
    if (auto credProps = object.getObject("credProps"_s)) {
        if (auto rk = credProps->getBoolean("rk"_s)) {
            outputs.credProps = CredentialPropertiesOutput { *rk };
            hasOutputs = true;
        }
    }
    if (auto largeBlob = object.getObject("largeBlob"_s)) {
        AuthenticationExtensionsClientOutputs::LargeBlobOutputs largeBlobOutputs;
        bool hasLargeBlob = false;
        if (auto supported = largeBlob->getBoolean("supported"_s)) {
            largeBlobOutputs.supported = *supported;
            hasLargeBlob = true;
        }
        if (auto written = largeBlob->getBoolean("written"_s)) {
            largeBlobOutputs.written = *written;
            hasLargeBlob = true;
        }
        if (hasLargeBlob) {
            outputs.largeBlob = WTF::move(largeBlobOutputs);
            hasOutputs = true;
        }
    }
    if (!hasOutputs)
        return std::nullopt;
    return outputs;
}

static AuthenticatorAttachment authenticatorAttachmentFromJSON(const JSON::Object& object)
{
    if (object.getString("authenticatorAttachment"_s) == "platform"_s)
        return AuthenticatorAttachment::Platform;
    return AuthenticatorAttachment::CrossPlatform;
}

static std::optional<AuthenticatorResponseData> parseRegistrationResponse(const JSON::Object& object)
{
    auto response = object.getObject("response"_s);
    if (!response)
        return std::nullopt;

    AuthenticatorResponseData data;
    data.isAuthenticatorAttestationResponse = true;
    data.rawId = arrayBufferFromBase64URLField(object, "rawId"_s);
    data.clientDataJSON = arrayBufferFromBase64URLField(*response, "clientDataJSON"_s);
    data.attestationObject = arrayBufferFromBase64URLField(*response, "attestationObject"_s);
    if (!data.rawId || !data.clientDataJSON || !data.attestationObject)
        return std::nullopt;

    if (auto transports = response->getArray("transports"_s)) {
        for (size_t i = 0; i < transports->length(); ++i) {
            if (auto transport = authenticatorTransportFromString(transports->get(i)->asString()))
                data.transports.append(*transport);
        }
    }

    if (auto extensionResults = object.getObject("clientExtensionResults"_s))
        data.extensionOutputs = extensionOutputsFromJSON(*extensionResults);

    return data;
}

static std::optional<AuthenticatorResponseData> parseAuthenticationResponse(const JSON::Object& object)
{
    auto response = object.getObject("response"_s);
    if (!response)
        return std::nullopt;

    AuthenticatorResponseData data;
    data.isAuthenticatorAttestationResponse = false;
    data.rawId = arrayBufferFromBase64URLField(object, "rawId"_s);
    data.clientDataJSON = arrayBufferFromBase64URLField(*response, "clientDataJSON"_s);
    data.authenticatorData = arrayBufferFromBase64URLField(*response, "authenticatorData"_s);
    data.signature = arrayBufferFromBase64URLField(*response, "signature"_s);
    if (!data.rawId || !data.clientDataJSON || !data.authenticatorData || !data.signature)
        return std::nullopt;

    data.userHandle = arrayBufferFromBase64URLField(*response, "userHandle"_s);

    if (auto extensionResults = object.getObject("clientExtensionResults"_s))
        data.extensionOutputs = extensionOutputsFromJSON(*extensionResults);

    return data;
}

static ExceptionData exceptionFromCredentialsError(const String& error)
{
    static constexpr std::pair<ASCIILiteral, ExceptionCode> errorCodes[] = {
        { "AbortError"_s, ExceptionCode::AbortError },
        { "ConstraintError"_s, ExceptionCode::ConstraintError },
        { "InvalidStateError"_s, ExceptionCode::InvalidStateError },
        { "NotSupportedError"_s, ExceptionCode::NotSupportedError },
        { "SecurityError"_s, ExceptionCode::SecurityError },
        { "TypeError"_s, ExceptionCode::TypeError },
    };
    if (!error.isEmpty()) {
        for (const auto& [name, code] : errorCodes) {
            if (error.contains(name))
                return { code, error };
        }
    }
    return { ExceptionCode::NotAllowedError, error.isEmpty() ? "Operation failed."_s : error };
}

struct CredentialRequestContext {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(CredentialRequestContext);

    bool isGetAssertion { false };
    GRefPtr<GVariant> parameters;
    GRefPtr<GCancellable> cancellable;
    RequestCompletionHandler handler;
};

static void credentialCallReadyCallback(GObject* source, GAsyncResult* result, gpointer userData)
{
    auto context = std::unique_ptr<CredentialRequestContext>(static_cast<CredentialRequestContext*>(userData));

    GUniqueOutPtr<GError> error;
    GRefPtr<GVariant> reply = adoptGRef(g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error.outPtr()));
    if (!reply) {
        if (g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            context->handler({ }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::AbortError, "This request has been aborted."_s });
            return;
        }
        context->handler({ }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::NotAllowedError, makeString("Failed to complete the credentials request: "_s, String::fromUTF8(error->message)) });
        return;
    }

    guint32 responseCode;
    GVariant* rawResults = nullptr;
    g_variant_get(reply.get(), "(u@a{sv})", &responseCode, &rawResults);
    GRefPtr<GVariant> results = adoptGRef(rawResults);

    if (responseCode) {
        const char* errorMessage = nullptr;
        g_variant_lookup(results.get(), "error", "&s", &errorMessage);
        context->handler({ }, AuthenticatorAttachment::CrossPlatform, exceptionFromCredentialsError(String::fromUTF8(errorMessage)));
        return;
    }

    GVariant* rawPublicKey = nullptr;
    g_variant_lookup(results.get(), "public_key", "@a{sv}", &rawPublicKey);
    GRefPtr<GVariant> publicKey = adoptGRef(rawPublicKey);

    const char* responseJSON = nullptr;
    if (publicKey)
        g_variant_lookup(publicKey.get(), context->isGetAssertion ? "authentication_response_json" : "registration_response_json", "&s", &responseJSON);

    RefPtr<JSON::Object> responseObject;
    if (responseJSON) {
        if (auto value = JSON::Value::parseJSON(String::fromUTF8(responseJSON)))
            responseObject = value->asObject();
    }
    if (!responseObject) {
        context->handler({ }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::NotAllowedError, "Received an invalid response from the credentials service."_s });
        return;
    }

    auto data = context->isGetAssertion ? parseAuthenticationResponse(*responseObject) : parseRegistrationResponse(*responseObject);
    if (!data) {
        context->handler({ }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::NotAllowedError, "Received an invalid response from the credentials service."_s });
        return;
    }

    context->handler(*data, authenticatorAttachmentFromJSON(*responseObject), { });
}

static void credentialsBusGotCallback(GObject*, GAsyncResult* result, gpointer userData)
{
    auto context = std::unique_ptr<CredentialRequestContext>(static_cast<CredentialRequestContext*>(userData));

    GUniqueOutPtr<GError> error;
    GRefPtr<GDBusConnection> connection = adoptGRef(g_bus_get_finish(result, &error.outPtr()));
    if (!connection) {
        context->handler({ }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::NotAllowedError, makeString("Failed to connect to the session bus: "_s, String::fromUTF8(error->message)) });
        return;
    }

    auto* rawContext = context.release();
    g_dbus_connection_call(connection.get(), credentialsServiceName, credentialsObjectPath, credentialsInterfaceName,
        rawContext->isGetAssertion ? "GetCredential" : "CreateCredential", rawContext->parameters.get(),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, credentialsCallTimeout, rawContext->cancellable.get(),
        credentialCallReadyCallback, rawContext);
}

void WebAuthenticatorCoordinatorProxy::performRequest(WebAuthenticationRequestData&& requestData, RequestCompletionHandler&& handler)
{
    if (requestData.mediation == MediationRequirement::Conditional) {
        handler({ }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::NotSupportedError, "Conditional mediation is not supported."_s });
        return;
    }

    if (!requestData.frameInfo) {
        handler({ }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::InvalidStateError, { } });
        return;
    }

    auto origin = requestData.frameInfo->securityOrigin.toString();
    String topOrigin;
    if (requestData.parentOrigin)
        topOrigin = requestData.parentOrigin->toString();

    bool isGetAssertion = std::holds_alternative<PublicKeyCredentialRequestOptions>(requestData.options);
    auto requestJSON = WTF::switchOn(requestData.options, [](const PublicKeyCredentialCreationOptions& options) {
        return serializeMakeCredentialOptions(options);
    }, [](const PublicKeyCredentialRequestOptions& options) {
        return serializeGetAssertionOptions(options);
    });

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "public_key", g_variant_new_string(requestJSON.utf8().data()));
    if (!topOrigin.isNull())
        g_variant_builder_add(&options, "{sv}", "top_origin", g_variant_new_string(topOrigin.utf8().data()));

    GRefPtr<GVariant> parameters;
    if (isGetAssertion)
        parameters = g_variant_new("(ss@a{sv}s)", "", origin.utf8().data(), g_variant_builder_end(&options), webAuthnApplicationID());
    else
        parameters = g_variant_new("(sss@a{sv}s)", "", origin.utf8().data(), "publicKey", g_variant_builder_end(&options), webAuthnApplicationID());

    m_cancellable = adoptGRef(g_cancellable_new());

    auto context = makeUnique<CredentialRequestContext>();
    context->isGetAssertion = isGetAssertion;
    context->parameters = WTF::move(parameters);
    context->cancellable = m_cancellable;
    context->handler = WTF::move(handler);
    g_bus_get(G_BUS_TYPE_SESSION, m_cancellable.get(), credentialsBusGotCallback, context.release());
}

void WebAuthenticatorCoordinatorProxy::cancel(CompletionHandler<void()>&& completionHandler)
{
    if (m_cancellable)
        g_cancellable_cancel(m_cancellable.get());
    completionHandler();
}

void WebAuthenticatorCoordinatorProxy::isUserVerifyingPlatformAuthenticatorAvailable(const SecurityOriginData&, QueryCompletionHandler&& handler)
{
    handler(false);
}

void WebAuthenticatorCoordinatorProxy::isConditionalMediationAvailable(const SecurityOriginData&, QueryCompletionHandler&& handler)
{
    handler(false);
}

void WebAuthenticatorCoordinatorProxy::getClientCapabilities(const SecurityOriginData&, CapabilitiesCompletionHandler&& handler)
{
    Vector<KeyValuePair<String, bool>> capabilities;
    capabilities.append({ "conditionalCreate"_s, false });
    capabilities.append({ "conditionalGet"_s, false });
    capabilities.append({ "hybridTransport"_s, true });
    capabilities.append({ "passkeyPlatformAuthenticator"_s, true });
    capabilities.append({ "userVerifyingPlatformAuthenticator"_s, false });
    capabilities.append({ "relatedOrigins"_s, true });
    capabilities.append({ "signalAllAcceptedCredentials"_s, false });
    capabilities.append({ "signalCurrentUserDetails"_s, false });
    capabilities.append({ "signalUnknownCredential"_s, false });
    handler(WTF::move(capabilities));
}

void WebAuthenticatorCoordinatorProxy::signalUnknownCredential(const SecurityOriginData&, UnknownCredentialOptions&&, CompletionHandler<void(std::optional<ExceptionData>)>&& handler)
{
    handler(ExceptionData { ExceptionCode::NotSupportedError, "Not implemented."_s });
}

void WebAuthenticatorCoordinatorProxy::signalAllAcceptedCredentials(const SecurityOriginData&, AllAcceptedCredentialsOptions&&, CompletionHandler<void(std::optional<ExceptionData>)>&& handler)
{
    handler(ExceptionData { ExceptionCode::NotSupportedError, "Not implemented."_s });
}

void WebAuthenticatorCoordinatorProxy::signalCurrentUserDetails(const SecurityOriginData&, CurrentUserDetailsOptions&&, CompletionHandler<void(std::optional<ExceptionData>)>&& handler)
{
    handler(ExceptionData { ExceptionCode::NotSupportedError, "Not implemented."_s });
}

} // namespace WebKit

#endif // ENABLE(WEB_AUTHN)
