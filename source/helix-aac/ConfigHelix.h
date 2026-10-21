#pragma once
/* r34view build settings for the vendored Helix AAC decoder (replaces the
 * Arduino wrapper's ConfigHelix.h). Plain stdlib; SBR on for HE-AAC streams. */
#ifndef USE_DEFAULT_STDLIB
#define USE_DEFAULT_STDLIB
#endif
#define HELIX_FEATURE_AUDIO_CODEC_AAC_SBR
