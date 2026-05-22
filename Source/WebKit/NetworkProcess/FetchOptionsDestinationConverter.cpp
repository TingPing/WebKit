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
#include "FetchOptionsDestinationConverter.h"

#if ENABLE(COMPRESSION_DICTIONARY_TRANSPORT)

#include <wtf/NeverDestroyed.h>
#include <wtf/SortedArrayMap.h>

namespace WebKit {

namespace FetchOptionsDestinationConverter {

// FIXME: This duplicates generated helpers from JSFetchRequestDestination.h.

String convertEnumerationToString(WebCore::FetchOptionsDestination enumerationValue)
{
    static const std::array<NeverDestroyed<String>, 25> values {
        emptyString(),
        MAKE_STATIC_STRING_IMPL("audio"),
        MAKE_STATIC_STRING_IMPL("audioworklet"),
        MAKE_STATIC_STRING_IMPL("compression-dictionary"),
        MAKE_STATIC_STRING_IMPL("document"),
        MAKE_STATIC_STRING_IMPL("embed"),
        MAKE_STATIC_STRING_IMPL("environmentmap"),
        MAKE_STATIC_STRING_IMPL("font"),
        MAKE_STATIC_STRING_IMPL("iframe"),
        MAKE_STATIC_STRING_IMPL("image"),
        MAKE_STATIC_STRING_IMPL("json"),
        MAKE_STATIC_STRING_IMPL("manifest"),
        MAKE_STATIC_STRING_IMPL("model"),
        MAKE_STATIC_STRING_IMPL("object"),
        MAKE_STATIC_STRING_IMPL("paintworklet"),
        MAKE_STATIC_STRING_IMPL("report"),
        MAKE_STATIC_STRING_IMPL("script"),
        MAKE_STATIC_STRING_IMPL("serviceworker"),
        MAKE_STATIC_STRING_IMPL("sharedworker"),
        MAKE_STATIC_STRING_IMPL("speculationrules"),
        MAKE_STATIC_STRING_IMPL("style"),
        MAKE_STATIC_STRING_IMPL("track"),
        MAKE_STATIC_STRING_IMPL("video"),
        MAKE_STATIC_STRING_IMPL("worker"),
        MAKE_STATIC_STRING_IMPL("xslt"),
    };
    return values[static_cast<size_t>(enumerationValue)];
}

std::optional<WebCore::FetchOptionsDestination> parseEnumerationFromString(const String& stringValue)
{
    if (stringValue.isEmpty())
        return WebCore::FetchRequestDestination::EmptyString;
    static constexpr SortedArrayMap enumerationMapping { WTF::toArray<std::pair<ComparableASCIILiteral, FetchRequestDestination>>({
        { "audio"_s, FetchOptionsDestination::Audio },
        { "audioworklet"_s, FetchOptionsDestination::Audioworklet },
        { "compression-dictionary"_s, FetchOptionsDestination::CompressionDictionary },
        { "document"_s, FetchOptionsDestination::Document },
        { "embed"_s, FetchOptionsDestination::Embed },
        { "environmentmap"_s, FetchOptionsDestination::Environmentmap },
        { "font"_s, FetchOptionsDestination::Font },
        { "iframe"_s, FetchOptionsDestination::Iframe },
        { "image"_s, FetchOptionsDestination::Image },
        { "json"_s, FetchOptionsDestination::Json },
        { "manifest"_s, FetchOptionsDestination::Manifest },
        { "model"_s, FetchOptionsDestination::Model },
        { "object"_s, FetchOptionsDestination::Object },
        { "paintworklet"_s, FetchOptionsDestination::Paintworklet },
        { "report"_s, FetchOptionsDestination::Report },
        { "script"_s, FetchOptionsDestination::Script },
        { "serviceworker"_s, FetchOptionsDestination::Serviceworker },
        { "sharedworker"_s, FetchOptionsDestination::Sharedworker },
        { "speculationrules"_s, FetchOptionsDestination::Speculationrules },
        { "style"_s, FetchOptionsDestination::Style },
        { "track"_s, FetchOptionsDestination::Track },
        { "video"_s, FetchOptionsDestination::Video },
        { "worker"_s, FetchOptionsDestination::Worker },
        { "xslt"_s, FetchOptionsDestination::Xslt },
    }) };
    if (auto* enumerationValue = enumerationMapping.tryGet(stringValue); enumerationValue) [[likely]]
        return *enumerationValue;
    return std::nullopt;
}

} // namespace FetchOptionsDestinationConverter

} // namespace WebKit

#endif // ENABLE(COMPRESSION_DICTIONARY_TRANSPORT)
