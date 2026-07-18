/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
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
#include "URLPatternComponent.h"

#include "ExceptionOr.h"
#include "URLPatternCanonical.h"
#include "URLPatternParser.h"
#include "URLPatternResult.h"
#include <JavaScriptCore/YarrFlags.h>
#include <JavaScriptCore/YarrInterpreter.h>
#include <JavaScriptCore/YarrPattern.h>
#include <wtf/BumpPointerAllocator.h>
#include <wtf/text/MakeString.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {
namespace URLPatternUtilities {

// Parsed representation of a component used to match without a regular-expression engine.
// The delimiter and ignoreCase are those the part list was parsed with, and are needed to
// reproduce segment-wildcard and case-folding semantics at match time.
struct StructuralPattern {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(StructuralPattern);

    Vector<Part> partList;
    String delimiterCodepoint;
    bool ignoreCase { false };
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(URLPatternComponent::CompiledPattern);
WTF_MAKE_TZONE_ALLOCATED_IMPL(StructuralPattern);

URLPatternComponent::URLPatternComponent() = default;

URLPatternComponent::~URLPatternComponent() = default;

URLPatternComponent::URLPatternComponent(URLPatternComponent&&) = default;
URLPatternComponent& URLPatternComponent::operator=(URLPatternComponent&&) = default;

URLPatternComponent::URLPatternComponent(String&& patternString, std::unique_ptr<CompiledPattern>&& compiled, Vector<String>&& groupNameList, bool hasRegexpGroupsFromPartsList)
    : m_patternString(WTF::move(patternString))
    , m_compiledPattern(WTF::move(compiled))
    , m_groupNameList(WTF::move(groupNameList))
    , m_hasRegexGroupsFromPartList(hasRegexpGroupsFromPartsList)
{
}

URLPatternComponent::URLPatternComponent(String&& patternString, std::unique_ptr<StructuralPattern>&& structural, Vector<String>&& groupNameList, bool hasRegexpGroupsFromPartsList)
    : m_patternString(WTF::move(patternString))
    , m_structuralPattern(WTF::move(structural))
    , m_groupNameList(WTF::move(groupNameList))
    , m_hasRegexGroupsFromPartList(hasRegexpGroupsFromPartsList)
{
}

// https://urlpattern.spec.whatwg.org/#compile-a-component
ExceptionOr<URLPatternComponent> URLPatternComponent::compile(StringView input, EncodingCallbackType type, const URLPatternStringOptions& options)
{
    auto maybePartList = URLPatternParser::parse(input, options, type);
    if (maybePartList.hasException())
        return maybePartList.releaseException();
    Vector<Part> partList = maybePartList.releaseReturnValue();

    auto [regularExpressionString, nameList] = generateRegexAndNameList(partList, options);

    OptionSet<JSC::Yarr::Flags> flags = { JSC::Yarr::Flags::UnicodeSets };
    if (options.ignoreCase)
        flags.add(JSC::Yarr::Flags::IgnoreCase);

    JSC::Yarr::ErrorCode errorCode = JSC::Yarr::ErrorCode::NoError;
    JSC::Yarr::YarrPattern yarrPattern(regularExpressionString, flags, errorCode);
    if (JSC::Yarr::hasError(errorCode))
        return Exception { ExceptionCode::TypeError, makeString("Unable to create RegExp object regular expression from provided URLPattern string: "_s, JSC::Yarr::errorMessage(errorCode)) };

    auto allocator = makeUnique<WTF::BumpPointerAllocator>();
    auto bytecode = JSC::Yarr::byteCompile(yarrPattern, allocator.get(), errorCode, nullptr);
    if (JSC::Yarr::hasError(errorCode) || !bytecode)
        return Exception { ExceptionCode::TypeError, "Unable to compile RegExp bytecode from provided URLPattern string."_s };

    String patternString = generatePatternString(partList, options);
    bool hasRegexGroups = partList.containsIf([](auto& part) {
        return part.type == PartType::Regexp;
    });

    auto compiled = WTF::makeUnique<CompiledPattern>(CompiledPattern { WTF::move(allocator), WTF::move(bytecode) });

    return URLPatternComponent { WTF::move(patternString), WTF::move(compiled), WTF::move(nameList), hasRegexGroups };
}

ExceptionOr<URLPatternComponent> URLPatternComponent::compileWithoutRegExp(StringView input, EncodingCallbackType type, const URLPatternStringOptions& options)
{
    auto maybePartList = URLPatternParser::parse(input, options, type);
    if (maybePartList.hasException())
        return maybePartList.releaseException();
    Vector<Part> partList = maybePartList.releaseReturnValue();

    // A regexp group cannot be matched without a regular-expression engine. Fail as early as
    // possible — before doing any further work — so createWithoutRegExpSupport() rejects such a
    // pattern outright rather than producing one that can never be matched.
    bool hasRegexGroups = partList.containsIf([](auto& part) {
        return part.type == PartType::Regexp;
    });
    if (hasRegexGroups)
        return Exception { ExceptionCode::TypeError, "URLPattern contains a regular expression group, which is not supported without a regular-expression engine."_s };

    Vector<String> nameList;
    for (auto& part : partList) {
        if (part.type != PartType::FixedText)
            nameList.append(part.name);
    }

    String patternString = generatePatternString(partList, options);

    if (options.ignoreCase) {
        // Match case-insensitively by case-folding the literal parts (prefix, fixed value, suffix)
        // up front; the input string is folded to match in matchesWithoutRegExp().
        for (auto& part : partList) {
            part.prefix = part.prefix.foldCase();
            part.value = part.value.foldCase();
            part.suffix = part.suffix.foldCase();
        }
    }

    auto structural = makeUnique<StructuralPattern>(StructuralPattern { WTF::move(partList), options.delimiterCodepoint, options.ignoreCase });

    return URLPatternComponent { WTF::move(patternString), WTF::move(structural), WTF::move(nameList), hasRegexGroups };
}

static bool isComponentDelimiter(char16_t character, StringView delimiterCodepoint)
{
    // The delimiter, when present, is always a single ASCII code point ("/" for paths, "." for hosts).
    return !delimiterCodepoint.isEmpty() && character == delimiterCodepoint[0];
}

static bool literalMatchesAt(StringView input, unsigned position, StringView literal, unsigned& endPosition)
{
    if (position > input.length() || literal.length() > input.length() - position)
        return false;

    // Both the input and the literal have already been case-folded when the component was compiled
    // with ignoreCase, so this comparison is always case-sensitive.
    if (StringView(input).substring(position, literal.length()) != literal)
        return false;

    endPosition = position + literal.length();
    return true;
}

// Appends every end position reachable by matching exactly one occurrence of `part` (prefix, then
// its core, then suffix) starting at `start`. This mirrors how generateRegexAndNameList() would
// encode the part, but evaluated directly against the input rather than via a regular expression.
static void appendSingleOccurrenceEnds(const Part& part, StringView input, unsigned start, StringView delimiterCodepoint, Vector<unsigned>& ends)
{
    unsigned length = input.length();

    unsigned afterPrefix;
    if (!literalMatchesAt(input, start, part.prefix, afterPrefix))
        return;

    auto appendWithSuffix = [&](unsigned afterCore) {
        unsigned afterSuffix;
        if (literalMatchesAt(input, afterCore, part.suffix, afterSuffix))
            ends.append(afterSuffix);
    };

    switch (part.type) {
    case PartType::FixedText: {
        unsigned afterCore;
        if (literalMatchesAt(input, afterPrefix, part.value, afterCore))
            appendWithSuffix(afterCore);
        break;
    }
    case PartType::SegmentWildcard: {
        // Matches one or more code units, none of which is the component delimiter.
        for (unsigned position = afterPrefix; position < length && !isComponentDelimiter(input[position], delimiterCodepoint); ) {
            ++position;
            appendWithSuffix(position);
        }
        break;
    }
    case PartType::FullWildcard: {
        // Matches zero or more code units of any kind.
        for (unsigned position = afterPrefix; position <= length; ++position)
            appendWithSuffix(position);
        break;
    }
    case PartType::Regexp:
        // Cannot be matched without a regular-expression engine; callers reject via hasRegexGroupsFromPartList().
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
}

static void markReachableEnds(const Part& part, StringView input, unsigned start, StringView delimiterCodepoint, Vector<bool>& reachable)
{
    switch (part.modifier) {
    case Modifier::None: {
        Vector<unsigned> ends;
        appendSingleOccurrenceEnds(part, input, start, delimiterCodepoint, ends);
        for (unsigned end : ends)
            reachable[end] = true;
        return;
    }
    case Modifier::Optional: {
        reachable[start] = true;
        Vector<unsigned> ends;
        appendSingleOccurrenceEnds(part, input, start, delimiterCodepoint, ends);
        for (unsigned end : ends)
            reachable[end] = true;
        return;
    }
    case Modifier::ZeroOrMore:
    case Modifier::OneOrMore: {
        if (part.modifier == Modifier::ZeroOrMore)
            reachable[start] = true;

        Vector<bool> expanded(FillWith { }, input.length() + 1, false);
        Vector<unsigned> worklist;
        worklist.append(start);
        while (!worklist.isEmpty()) {
            unsigned position = worklist.takeLast();
            if (expanded[position])
                continue;
            expanded[position] = true;

            Vector<unsigned> ends;
            appendSingleOccurrenceEnds(part, input, position, delimiterCodepoint, ends);
            for (unsigned end : ends) {
                reachable[end] = true;
                if (!expanded[end])
                    worklist.append(end);
            }
        }
        return;
    }
    }
}

static bool matchPartListFrom(const Vector<Part>& parts, unsigned partIndex, unsigned position, StringView input, StringView delimiterCodepoint, Vector<std::optional<bool>>& memo)
{
    unsigned length = input.length();
    if (partIndex == parts.size())
        return position == length;

    unsigned key = partIndex * (length + 1) + position;
    if (memo[key])
        return *memo[key];

    Vector<bool> reachable(FillWith { }, length + 1, false);
    markReachableEnds(parts[partIndex], input, position, delimiterCodepoint, reachable);

    bool result = false;
    for (unsigned end = 0; end <= length && !result; ++end) {
        if (reachable[end] && matchPartListFrom(parts, partIndex + 1, end, input, delimiterCodepoint, memo))
            result = true;
    }

    memo[key] = result;
    return result;
}

bool URLPatternComponent::matchesWithoutRegExp(StringView input) const
{
    RELEASE_ASSERT(m_structuralPattern);
    ASSERT(!m_hasRegexGroupsFromPartList);

    String foldedInput;
    if (m_structuralPattern->ignoreCase) {
        foldedInput = input.toString().foldCase();
        input = foldedInput;
    }

    const auto& parts = m_structuralPattern->partList;
    unsigned length = input.length();
    Vector<std::optional<bool>> memo((parts.size() + 1) * (length + 1));
    return matchPartListFrom(parts, 0, 0, input, m_structuralPattern->delimiterCodepoint, memo);
}

static constexpr std::array specialSchemeList { "ftp"_s, "file"_s, "http"_s, "https"_s, "ws"_s, "wss"_s };

// https://urlpattern.spec.whatwg.org/#protocol-component-matches-a-special-scheme
bool URLPatternComponent::matchesSpecialSchemeProtocolWithoutRegExp() const
{
    return std::ranges::any_of(specialSchemeList, [this](auto scheme) {
        return matchesWithoutRegExp(StringView { scheme });
    });
}

// https://urlpattern.spec.whatwg.org/#protocol-component-matches-a-special-scheme
bool URLPatternComponent::matchSpecialSchemeProtocol() const
{
    return std::ranges::any_of(specialSchemeList, [this](const String& scheme) {
        return componentExec(scheme).has_value();
    });
}

std::optional<Vector<unsigned>> URLPatternComponent::componentExec(StringView comparedString) const
{
    if (!m_compiledPattern)
        return std::nullopt;

    // m_offsetsSize accounts for (numSubpatterns+1)*2 capture offsets plus any
    // additional slots for duplicate named capture groups.
    unsigned outputSize = m_compiledPattern->bytecode->m_offsetsSize;
    Vector<unsigned> output(outputSize);
    std::ranges::fill(output, std::numeric_limits<unsigned>::max());

    unsigned result = JSC::Yarr::interpret(m_compiledPattern->bytecode.get(), comparedString, 0, output.begin());
    if (result == std::numeric_limits<unsigned>::max())
        return std::nullopt;

    return output;
}

// https://urlpattern.spec.whatwg.org/#create-a-component-match-result
URLPatternComponentResult URLPatternComponent::createComponentMatchResult(String&& input, const Vector<unsigned>& offsets) const
{
    URLPatternComponentResult::GroupsRecord groups;

    ASSERT(offsets.size() >= (m_groupNameList.size() + 1) * 2);
    for (unsigned index = 0; index < m_groupNameList.size(); ++index) {
        unsigned start = offsets[(index + 1) * 2];
        unsigned end = offsets[(index + 1) * 2 + 1];

        Variant<std::monostate, String> value;
        if (start != std::numeric_limits<unsigned>::max() && end != std::numeric_limits<unsigned>::max())
            value = StringView(input).substring(start, end - start).toString();

        groups.append(URLPatternComponentResult::NameMatchPair { m_groupNameList[index], WTF::move(value) });
    }

    return URLPatternComponentResult { !input.isEmpty() ? WTF::move(input) : emptyString(), WTF::move(groups) };
}

}
}
