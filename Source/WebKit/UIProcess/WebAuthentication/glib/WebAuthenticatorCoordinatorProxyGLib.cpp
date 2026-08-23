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

#include "Logging.h"
#include "WebAuthenticationRequestData.h"
#include <WebCore/AuthenticatorAttachment.h>
#include <WebCore/AuthenticatorResponseData.h>
#include <WebCore/CredentialPropertiesOutput.h>
#include <WebCore/ExceptionData.h>
#include <WebCore/SecurityOriginData.h>
#include <WebCore/WebAuthenticationUtils.h>
#include <gio/gio.h>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/HexNumber.h>
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

static const char portalServiceName[] = "org.freedesktop.portal.Desktop";
static const char portalObjectPath[] = "/org/freedesktop/portal/desktop";
static const char portalCredentialInterfaceName[] = "org.freedesktop.portal.experimental.Credential";
static const char portalRequestInterfaceName[] = "org.freedesktop.portal.Request";
static const char portalHostRegistryInterfaceName[] = "org.freedesktop.host.portal.Registry";

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
    RELEASE_LOG_ERROR(WebAuthn, "Credentials request failed: %s", error.utf8().data());

    // SecurityError is deliberately not mapped: the service raises it for
    // caller trust failures, and reporting those the same as any other
    // disallowed request avoids leaking the distinction to web content.
    static constexpr std::tuple<ASCIILiteral, ExceptionCode, ASCIILiteral> errorCodes[] = {
        { "AbortError"_s, ExceptionCode::AbortError, "This request has been aborted."_s },
        { "ConstraintError"_s, ExceptionCode::ConstraintError, "The operation failed due to an unsatisfiable constraint."_s },
        { "InvalidStateError"_s, ExceptionCode::InvalidStateError, "The authenticator already contains one of the requested credentials."_s },
        { "NotSupportedError"_s, ExceptionCode::NotSupportedError, "The requested option is not supported."_s },
        { "TypeError"_s, ExceptionCode::TypeError, "The request is invalid."_s },
    };
    for (const auto& [name, code, message] : errorCodes) {
        if (error.contains(name))
            return { code, message };
    }
    return { ExceptionCode::NotAllowedError, "The request is not allowed."_s };
}

struct CredentialRequestContext {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(CredentialRequestContext);

    bool isGetAssertion { false };
    String origin;
    String topOrigin;
    String requestJSON;
    GRefPtr<GDBusConnection> connection;
    GRefPtr<GCancellable> cancellable;
    CString requestPath;
    unsigned responseSubscription { 0 };
    unsigned timeoutID { 0 };
    gulong cancelledID { 0 };
    RequestCompletionHandler handler;
};

static void completeCredentialRequest(CredentialRequestContext* rawContext, const AuthenticatorResponseData& data, AuthenticatorAttachment attachment, const ExceptionData& exception)
{
    auto context = std::unique_ptr<CredentialRequestContext>(rawContext);
    if (context->responseSubscription)
        g_dbus_connection_signal_unsubscribe(context->connection.get(), std::exchange(context->responseSubscription, 0));
    if (context->timeoutID)
        g_source_remove(std::exchange(context->timeoutID, 0));
    if (context->cancelledID)
        g_signal_handler_disconnect(context->cancellable.get(), std::exchange(context->cancelledID, 0));
    context->handler(data, attachment, exception);
}

static void handleCredentialResponse(CredentialRequestContext* context, unsigned responseCode, GVariant* results)
{
    if (responseCode == 1) {
        completeCredentialRequest(context, { }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::NotAllowedError, "This request has been cancelled by the user."_s });
        return;
    }

    if (responseCode) {
        const char* errorMessage = nullptr;
        g_variant_lookup(results, "error", "&s", &errorMessage);
        completeCredentialRequest(context, { }, AuthenticatorAttachment::CrossPlatform, exceptionFromCredentialsError(String::fromUTF8(errorMessage)));
        return;
    }

    GVariant* rawPublicKey = nullptr;
    g_variant_lookup(results, "public_key", "@a{sv}", &rawPublicKey);
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
        completeCredentialRequest(context, { }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::NotAllowedError, "Received an invalid response from the credentials service."_s });
        return;
    }

    auto data = context->isGetAssertion ? parseAuthenticationResponse(*responseObject) : parseRegistrationResponse(*responseObject);
    if (!data) {
        completeCredentialRequest(context, { }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::NotAllowedError, "Received an invalid response from the credentials service."_s });
        return;
    }

    completeCredentialRequest(context, *data, authenticatorAttachmentFromJSON(*responseObject), { });
}

// Returns a floating reference, consumed by g_variant_new() at the call sites.
static GVariant* credentialRequestOptions(CredentialRequestContext* context, const String* handleToken = nullptr)
{
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    if (handleToken)
        g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(handleToken->utf8().data()));
    g_variant_builder_add(&options, "{sv}", "public_key", g_variant_new_string(context->requestJSON.utf8().data()));
    if (!context->topOrigin.isNull())
        g_variant_builder_add(&options, "{sv}", "top_origin", g_variant_new_string(context->topOrigin.utf8().data()));
    return g_variant_builder_end(&options);
}

// Development builds of credentialsd can be configured to accept direct calls
// on its gateway interface with the CREDSD_TRUSTED_CALLERS and
// CREDSD_TRUSTED_APP_IDS environment variables, which allows testing without
// a Credential portal.
static void credentialsServiceCallReadyCallback(GObject* source, GAsyncResult* result, gpointer userData)
{
    auto* context = static_cast<CredentialRequestContext*>(userData);

    GUniqueOutPtr<GError> error;
    GRefPtr<GVariant> reply = adoptGRef(g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error.outPtr()));
    if (!reply) {
        if (g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            completeCredentialRequest(context, { }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::AbortError, "This request has been aborted."_s });
            return;
        }
        RELEASE_LOG_ERROR(WebAuthn, "Failed to complete the credentials request: %s", error->message);
        completeCredentialRequest(context, { }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::NotAllowedError, "The request is not allowed."_s });
        return;
    }

    guint32 responseCode;
    GVariant* rawResults = nullptr;
    g_variant_get(reply.get(), "(u@a{sv})", &responseCode, &rawResults);
    GRefPtr<GVariant> results = adoptGRef(rawResults);
    handleCredentialResponse(context, responseCode, results.get());
}

static void callCredentialsService(CredentialRequestContext* context)
{
    GRefPtr<GVariant> parameters;
    if (context->isGetAssertion)
        parameters = g_variant_new("(ss@a{sv}s)", "", context->origin.utf8().data(), credentialRequestOptions(context), webAuthnApplicationID());
    else
        parameters = g_variant_new("(sss@a{sv}s)", "", context->origin.utf8().data(), "publicKey", credentialRequestOptions(context), webAuthnApplicationID());

    g_dbus_connection_call(context->connection.get(), credentialsServiceName, credentialsObjectPath, credentialsInterfaceName,
        context->isGetAssertion ? "GetCredential" : "CreateCredential", parameters.get(),
        G_VARIANT_TYPE("(ua{sv})"), G_DBUS_CALL_FLAGS_NONE, credentialsCallTimeout, context->cancellable.get(),
        credentialsServiceCallReadyCallback, context);
}

static void closePortalRequest(CredentialRequestContext* context)
{
    g_dbus_connection_call(context->connection.get(), portalServiceName, context->requestPath.data(), portalRequestInterfaceName,
        "Close", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

static void portalResponseCallback(GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* parameters, gpointer userData)
{
    auto* context = static_cast<CredentialRequestContext*>(userData);

    guint32 responseCode;
    GVariant* rawResults = nullptr;
    g_variant_get(parameters, "(u@a{sv})", &responseCode, &rawResults);
    GRefPtr<GVariant> results = adoptGRef(rawResults);
    handleCredentialResponse(context, responseCode, results.get());
}

static void portalCallReadyCallback(GObject* source, GAsyncResult* result, gpointer userData)
{
    auto* context = static_cast<CredentialRequestContext*>(userData);

    GUniqueOutPtr<GError> error;
    GRefPtr<GVariant> reply = adoptGRef(g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error.outPtr()));
    if (!reply) {
        if (g_error_matches(error.get(), G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            completeCredentialRequest(context, { }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::AbortError, "This request has been aborted."_s });
            return;
        }
        if (g_error_matches(error.get(), G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)
            || g_error_matches(error.get(), G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE)
            || g_error_matches(error.get(), G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT)
            || g_error_matches(error.get(), G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)) {
            RELEASE_LOG(WebAuthn, "Credential portal is not available, calling the credentials service directly: %s", error->message);
            if (context->responseSubscription)
                g_dbus_connection_signal_unsubscribe(context->connection.get(), std::exchange(context->responseSubscription, 0));
            callCredentialsService(context);
            return;
        }
        RELEASE_LOG_ERROR(WebAuthn, "Failed to complete the credential portal request: %s", error->message);
        completeCredentialRequest(context, { }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::NotAllowedError, "The request is not allowed."_s });
        return;
    }

    const char* handle = nullptr;
    g_variant_get(reply.get(), "(&o)", &handle);
    if (context->requestPath != handle) {
        g_dbus_connection_signal_unsubscribe(context->connection.get(), std::exchange(context->responseSubscription, 0));
        context->requestPath = handle;
        context->responseSubscription = g_dbus_connection_signal_subscribe(context->connection.get(), portalServiceName,
            portalRequestInterfaceName, "Response", context->requestPath.data(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            portalResponseCallback, context, nullptr);
    }

    context->timeoutID = g_timeout_add_seconds(credentialsCallTimeout / 1000, [](gpointer userData) -> gboolean {
        auto* context = static_cast<CredentialRequestContext*>(userData);
        context->timeoutID = 0;
        closePortalRequest(context);
        completeCredentialRequest(context, { }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::NotAllowedError, "Operation timed out."_s });
        return G_SOURCE_REMOVE;
    }, context);

    context->cancelledID = g_cancellable_connect(context->cancellable.get(), G_CALLBACK(+[](GCancellable*, gpointer userData) {
        auto* context = static_cast<CredentialRequestContext*>(userData);
        context->cancelledID = 0;
        closePortalRequest(context);
        completeCredentialRequest(context, { }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::AbortError, "This request has been aborted."_s });
    }), context, nullptr);
}

static void registerHostApplication(GDBusConnection* connection)
{
    static bool alreadyRegistered = false;
    if (alreadyRegistered)
        return;
    alreadyRegistered = true;

    // Host applications have to register their application ID for the portal
    // to forward it; sandboxed applications get theirs from the sandbox.
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_dbus_connection_call(connection, portalServiceName, portalObjectPath, portalHostRegistryInterfaceName, "Register",
        g_variant_new("(sa{sv})", webAuthnApplicationID(), &options), nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer) {
            GUniqueOutPtr<GError> error;
            GRefPtr<GVariant> reply = adoptGRef(g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error.outPtr()));
            if (!reply)
                RELEASE_LOG(WebAuthn, "Failed to register with the host portal registry: %s", error->message);
        }, nullptr);
}

static void callCredentialPortal(CredentialRequestContext* context)
{
    registerHostApplication(context->connection.get());

    auto token = makeString("webkit"_s, hex(cryptographicallyRandomNumber<uint64_t>()));
    auto sender = String::fromLatin1(g_dbus_connection_get_unique_name(context->connection.get()));
    context->requestPath = makeString("/org/freedesktop/portal/desktop/request/"_s, makeStringByReplacingAll(sender.substring(1), '.', '_'), '/', token).utf8();

    context->responseSubscription = g_dbus_connection_signal_subscribe(context->connection.get(), portalServiceName,
        portalRequestInterfaceName, "Response", context->requestPath.data(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        portalResponseCallback, context, nullptr);

    GRefPtr<GVariant> parameters;
    if (context->isGetAssertion)
        parameters = g_variant_new("(ss@a{sv})", "", context->origin.utf8().data(), credentialRequestOptions(context, &token));
    else
        parameters = g_variant_new("(sss@a{sv})", "", context->origin.utf8().data(), "publicKey", credentialRequestOptions(context, &token));

    g_dbus_connection_call(context->connection.get(), portalServiceName, portalObjectPath, portalCredentialInterfaceName,
        context->isGetAssertion ? "GetCredential" : "CreateCredential", parameters.get(),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, context->cancellable.get(),
        portalCallReadyCallback, context);
}

static void credentialsBusGotCallback(GObject*, GAsyncResult* result, gpointer userData)
{
    auto* context = static_cast<CredentialRequestContext*>(userData);

    GUniqueOutPtr<GError> error;
    GRefPtr<GDBusConnection> connection = adoptGRef(g_bus_get_finish(result, &error.outPtr()));
    if (!connection) {
        RELEASE_LOG_ERROR(WebAuthn, "Failed to connect to the session bus: %s", error->message);
        completeCredentialRequest(context, { }, AuthenticatorAttachment::CrossPlatform, ExceptionData { ExceptionCode::NotAllowedError, "The request is not allowed."_s });
        return;
    }

    context->connection = WTF::move(connection);
    callCredentialPortal(context);
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

    m_cancellable = adoptGRef(g_cancellable_new());

    auto context = makeUnique<CredentialRequestContext>();
    context->isGetAssertion = std::holds_alternative<PublicKeyCredentialRequestOptions>(requestData.options);
    context->origin = requestData.frameInfo->securityOrigin.toString();
    if (requestData.parentOrigin)
        context->topOrigin = requestData.parentOrigin->toString();
    context->requestJSON = WTF::switchOn(requestData.options, [](const PublicKeyCredentialCreationOptions& options) {
        return serializeMakeCredentialOptions(options);
    }, [](const PublicKeyCredentialRequestOptions& options) {
        return serializeGetAssertionOptions(options);
    });
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
    // Keys must be sorted in lexicographic order.
    Vector<KeyValuePair<String, bool>> capabilities;
    capabilities.append({ "conditionalCreate"_s, false });
    capabilities.append({ "conditionalGet"_s, false });
    capabilities.append({ "hybridTransport"_s, true });
    capabilities.append({ "passkeyPlatformAuthenticator"_s, true });
    capabilities.append({ "relatedOrigins"_s, true });
    capabilities.append({ "signalAllAcceptedCredentials"_s, false });
    capabilities.append({ "signalCurrentUserDetails"_s, false });
    capabilities.append({ "signalUnknownCredential"_s, false });
    capabilities.append({ "userVerifyingPlatformAuthenticator"_s, false });
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
