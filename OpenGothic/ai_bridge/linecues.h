#pragma once

#include <rapidjson/document.h>

#include <memory>
#include <string>
#include <string_view>

#include "graphics/mesh/speechcues.h"

// What the face of a line from the AI server does besides its mood: its facial gestures (say.cues, v61), the leak of
// a felt emotion through a lie (say.leak, v61) and the time of its words (v62). SpeechCues plays it.
namespace LineCues {
  // the time of a line's words in its sound: [[word, start s, end s], ...] of say, say_audio or say_end
  void readWords(const rapidjson::Value& msg, SpeechCues::Plan& plan);

  // the plan of a say: gestures [{"cue": name, "word": index of the word in its text}] - a one-sided one goes on a
  // side by the seed of the npc, the line and its place - and the leak {"felt", "half", "word"}; none: no gesture
  // and no leak. text: the line's text as the game holds it (its words as the server counts them)
  std::shared_ptr<SpeechCues::Plan> read(const rapidjson::Value& msg, const std::string& npc, const std::string& line,
                                         std::string_view text, bool streamed);
  }
