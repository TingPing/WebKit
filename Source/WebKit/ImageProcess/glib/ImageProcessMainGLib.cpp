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
#include "ImageProcessMain.h"

#if ENABLE(IMAGE_PROCESS) && (PLATFORM(GTK) || PLATFORM(WPE))
#include "AuxiliaryProcessMain.h"
#include "ImageProcess.h"
#include <WebCore/CommonAtomStrings.h>
#include <wtf/MainThread.h>
#include <wtf/Threading.h>
#endif

#if USE(SYSPROF_CAPTURE)
#include <wtf/SystemTracing.h>
#endif

namespace WebKit {

#if ENABLE(IMAGE_PROCESS) && (PLATFORM(GTK) || PLATFORM(WPE))
class ImageProcessMainGLib final : public AuxiliaryProcessMainBase<ImageProcess> {
public:
    bool platformInitialize() override
    {
#if USE(SYSPROF_CAPTURE)
        SysprofAnnotator::createIfNeeded("WebKit (Image)"_s);
#endif
        return true;
    }

    void initializeWebKit() override
    {
        WTF::initialize();
        WTF::initializeMainThread();
        WebCore::initializeCommonAtomStrings();
    }
};
#endif

int ImageProcessMain(int argc, char** argv)
{
#if ENABLE(IMAGE_PROCESS) && (PLATFORM(GTK) || PLATFORM(WPE))
    return AuxiliaryProcessMain<ImageProcessMainGLib>(argc, argv);
#else
    return 0;
#endif
}

} // namespace WebKit
