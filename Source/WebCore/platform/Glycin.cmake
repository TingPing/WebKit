list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    "${WEBCORE_DIR}/platform/graphics/glycin"
)

list(APPEND WebCore_SOURCES
    platform/graphics/glycin/ImageDecoderGlycin.cpp
)

list(APPEND WebCore_LIBRARIES
    Glycin::Glycin
)
