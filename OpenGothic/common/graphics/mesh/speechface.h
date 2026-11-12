#pragma once

#include "facerig.h"
#include "lipsync.h"

#include <rapidjson/fwd.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The face of a speaker from its voice (speech_face of the face profile; reports/content/2026-11-hero-head/
// engine-plan.md, step 10, "FaceRig"): a track of ARKit weights solved from the sound of a line by Audio2Face - the
// bridge's say_face, or <sound>.face.json for a voice of the game - plays on the head as the voice plays: frame i at
// i/fps s of the line's sound, the clock of LipSync (what the audio thread has played), linear between frames. The
// profile's rules turn its weights into the head's; FaceRig shows them over the other layers (setVoiceFace).
// It comes in over fade_in_ms from the start of the voice; at the end of its track or of the voice, or when the line is
// cut off, it holds its last frame and goes over fade_out_ms, and the mouth by loudness takes over again.
class SpeechFace final {
  public:
    // the track: the format shared by the service gothic-a2f, the server and the engine
    struct Track final {
      float                    fps    = 0;
      std::vector<std::string> shapes;            // ARKit names, the solver's order
      std::vector<uint8_t>     data;              // frames one after the other, a byte per shape: weight*255
      size_t                   frames = 0;
      std::string              emotion;           // the label it was made with ("anger:0.8,disgust:0.3"), empty: none
      bool                     emotional = false; // ...with a strength above 0

      float durationMs() const;  // the time of its last frame
      float weight(size_t frame, size_t shape) const { return float(data[frame*shapes.size()+shape])/255.f; }

      // {"fps", "shapes", "data" (base64), "emotion"?}, other fields aside; throws std::runtime_error with the reason
      static std::shared_ptr<const Track> read(const rapidjson::Value& obj);
      static std::shared_ptr<const Track> parse(std::string_view json);
      };

    // the emotions of Audio2Face v3.0 a label may name
    static constexpr std::string_view Emotions[] = {"amazement","anger","cheekiness","disgust","fear","grief","joy",
                                                    "outofbreath","pain","sadness"};
    static constexpr size_t Pending = 8;  // tracks of lines whose voices have not started yet, at most

    // OPENGOTHIC_SPEECH_FACE=0 switches the layer off: the face as without a track (a profile without speech_face
    // has none anyway)
    static bool isEnabled();
    static void setEnabled(bool e);

    // the profile's section; throws std::runtime_error with the reason
    static FaceProfile::Speech parseRules(const rapidjson::Value& v, const FaceProfile& profile);

    // the track of a voice of the game: "<the sound's name without .WAV>.face.json" (case aside) in the directory of
    // OPENGOTHIC_FACE_TRACKS, else in `dir` (the head's content); nullptr: none, or a broken one (Log::e)
    static std::shared_ptr<const Track> fromFile(std::string_view sound, std::string_view dir);

    // the track of a line by its id (say_face): the voice of that line (LipSync::Track::isOf) plays it, whether it
    // has started already or starts later
    void   setTrack(std::shared_ptr<const Track> track, std::string_view line);
    // a voice starts (MdlVisual::startSpeech): the track given for its line, else one of a file in `dir` (empty: no
    // files looked for); a face still shown from the last voice goes
    void   start(std::shared_ptr<LipSync::Track> voice, std::string_view dir, uint64_t now);
    // the line is cut off: the face goes over fade_out_ms
    void   stop(uint64_t now);
    // the face now into FaceRig::setVoiceFace; how far it shows, 0..1
    float  play(FaceRig& face, uint64_t now);

    bool   playing() const { return track!=nullptr; }  // a track is bound (it may be going)
    const Track* current() const { return track.get(); }
    // the weights of the track at `ms` of its sound, by the profile's rules: head target -> weight (tests, play)
    void   sample(const FaceRig& face, float ms, std::vector<std::pair<uint32_t,float>>& out);

  private:
    struct Line final {
      std::string                  id;
      std::shared_ptr<const Track> track;
      };
    struct Map final {
      std::vector<uint32_t>                target;  // per shape of the track, uint32_t(-1): not played
      std::vector<float>                   scale;
      std::vector<std::pair<size_t,size_t>> caps; // shape, the one it stays under
      };

    void     bind(std::shared_ptr<const Track> t);
    void     fadeOut(uint64_t now);
    void     remap(const FaceRig& face);
    uint64_t lineTime(uint64_t now) const;

    std::vector<Line>               pending;
    std::shared_ptr<LipSync::Track> voice;
    std::shared_ptr<const Track>    track;
    uint64_t                        startTick = 0;
    bool                            going     = false;  // the track is over: its last frame fades
    uint64_t                        goneAt    = 0;
    float                           goneK     = 0;
    float                           shownK    = 0;
    std::vector<std::pair<uint32_t,float>> shown;  // the weights as last shown

    const Track*                    mappedTrack   = nullptr;
    const FaceProfile*              mappedProfile = nullptr;
    Map                             map;
    std::vector<float>              raw;
  };
