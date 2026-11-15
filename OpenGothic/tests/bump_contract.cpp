#include "game/bumpsignal.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
  int failures = 0;

  void check(bool condition, const char* name) {
    if(!condition) {
      std::fprintf(stderr,"FAIL: %s\n",name);
      ++failures;
      }
    }

  bool near(float a, float b, float tolerance = 0.0001f) {
    return std::isfinite(a) && std::abs(a-b)<=tolerance;
    }

  struct Moves final {
    BumpSignal::State state;
    uint64_t now = 0;

    BumpSignal::Result move(float speed, uint64_t dt = 10, bool obstacle = false,
                            float acceptedFraction = 1.f, float angle = 0.f) {
      BumpSignal::Input input;
      input.nowMs = now+=dt;
      input.dtMs = dt;
      input.requested = {speed*float(dt)/1000.f,0,0};
      input.accepted = input.requested*acceptedFraction;
      const auto radians = angle*(3.14159265358979323846f/180.f);
      input.normal = {-std::cos(radians),0,std::sin(radians)};
      input.obstacle = obstacle;
      return BumpSignal::update(state,input);
      }

    void seed(float speed) {
      for(int i=0; i<30; ++i)
        move(speed);
      }

    BumpSignal::Result hit(float angle = 0.f) { return move(332,10,true,0,angle); }
    };

  void strength(float speed, float angle, float expected, const char* name) {
    Moves moves;
    moves.seed(speed);
    const auto result = moves.hit(angle);
    check(result.contactStarted && result.react==(expected>0) && near(result.avgSpeed,speed) &&
          near(result.strength,expected,0.01f) && near(result.angleDeg,angle,0.001f),name);
    std::printf("%s: avg=%.6f u=%.6f angle=%.3f lost=%.3f strength=%.6f react=%d\n",
                name,double(result.avgSpeed),double(result.speed),double(result.angleDeg),
                double(result.lost),double(result.strength),result.react);
    }
  }

int main() {
  strength(332,0,0.66f,"run_head_on");
  strength(196,0,0,"walk_head_on");
  strength(332,45,0.173797f,"run_45");
  strength(332,70,0,"run_70");

  Moves oscillating;
  oscillating.move(263,75);
  oscillating.move(435,25);
  oscillating.move(300,100);
  oscillating.move(400,100);
  const float weighted = (263.f*75+435.f*25+300.f*100+400.f*100)/300.f;
  Moves averaged;
  averaged.seed(weighted);
  const auto oscillation = oscillating.hit();
  const auto average = averaged.hit();
  check(near(oscillation.avgSpeed,weighted) && near(oscillation.strength,average.strength),"oscillating_speed");
  std::printf("oscillating_speed: avg=%.6f strength=%.6f constant_strength=%.6f\n",
              double(oscillation.avgSpeed),double(oscillation.strength),double(average.strength));

  Moves window;
  window.move(435,200);
  window.move(263,200);
  const auto partial = window.hit();
  check(near(partial.avgSpeed,(435.f*100+263.f*200)/300.f),"window_300_partial");
  std::printf("window_300_partial: avg=%.6f strength=%.6f\n",double(partial.avgSpeed),double(partial.strength));
  Moves ring;
  for(int i=0; i<350; ++i)
    ring.move(332,1);
  check(near(ring.hit().strength,0.66f),"ring_wrap");

  Moves held;
  held.seed(332);
  int reactions = 0, starts = 0;
  for(int i=0; i<10; ++i) {
    const auto hit = held.move(332,100,true,0.7f);
    reactions += hit.react ? 1 : 0;
    starts += hit.contactStarted ? 1 : 0;
    }
  check(reactions==1 && starts==1,"held_contact_10");
  std::printf("held_contact_10: reactions=%d starts=%d\n",reactions,starts);

  Moves flicker;
  flicker.seed(332);
  int flickerReactions = flicker.hit().react ? 1 : 0;
  flicker.move(332);
  const auto again = flicker.hit();
  flickerReactions += again.react ? 1 : 0;
  check(flickerReactions==1 && again.contactStarted && again.strength>0,"one_move_gap");
  std::printf("one_move_gap: reactions=%d second_start=%d second_strength=%.6f\n",
              flickerReactions,again.contactStarted,double(again.strength));

  Moves cooldown;
  cooldown.seed(332);
  check(cooldown.hit().react,"cooldown_first");
  for(int i=0; i<39; ++i)
    cooldown.move(332);
  const auto at400 = cooldown.hit();
  check(at400.contactStarted && !at400.react && at400.strength>0,"cooldown_400");
  for(int i=0; i<9; ++i)
    cooldown.move(332);
  const auto at500 = cooldown.hit();
  check(at500.contactStarted && at500.react,"cooldown_500");
  std::printf("cooldown: at400_react=%d at500_react=%d\n",at400.react,at500.react);

  Moves missing;
  missing.seed(332);
  const auto noObstacle = missing.move(332,10,false,0);
  check(!noObstacle.contactStarted && !noObstacle.react,"loss_without_obstacle");
  Moves smallLoss;
  smallLoss.seed(332);
  const auto lost20 = smallLoss.move(332,10,true,0.8f);
  check(near(lost20.lost,0.2f) && !lost20.contactStarted && !lost20.react,"loss_20_percent");
  const auto lost25 = smallLoss.move(332,10,true,0.75f);
  check(near(lost25.lost,0.25f) && lost25.contactStarted && lost25.react,"loss_25_percent");
  std::printf("loss_threshold: no_obstacle=%d lost20=%.6f start20=%d lost25=%.6f start25=%d\n",
              noObstacle.react,double(lost20.lost),lost20.contactStarted,double(lost25.lost),lost25.contactStarted);

  struct Gate final { const char* name; bool BumpSignal::Input::*member; bool value; };
  const Gate gates[] = {
    {"not_grounded",&BumpSignal::Input::grounded,false}, {"airborne",&BumpSignal::Input::airborne,true},
    {"falling",&BumpSignal::Input::falling,true}, {"sliding",&BumpSignal::Input::sliding,true},
    {"swimming",&BumpSignal::Input::swimming,true}, {"diving",&BumpSignal::Input::diving,true},
    {"jumping",&BumpSignal::Input::jumping,true}, {"attacking",&BumpSignal::Input::attacking,true},
    {"blocking",&BumpSignal::Input::blocking,true}, {"dodging",&BumpSignal::Input::dodging,true},
    {"rolling",&BumpSignal::Input::rolling,true}, {"melee_step",&BumpSignal::Input::meleeStep,true},
    {"down",&BumpSignal::Input::down,true}, {"ragdoll",&BumpSignal::Input::ragdoll,true},
    {"interactive",&BumpSignal::Input::interactive,true}, {"nonhuman",&BumpSignal::Input::human,false},
    {"flag_disabled",&BumpSignal::Input::enabled,false}
    };
  for(const auto& gate:gates) {
    Moves moves;
    moves.seed(332);
    BumpSignal::Input input;
    input.nowMs = moves.now+10;
    input.dtMs = 10;
    input.requested = {3.32f,0,0};
    input.normal = {-1,0,0};
    input.obstacle = true;
    input.*gate.member = gate.value;
    const auto result = BumpSignal::update(moves.state,input);
    check(!result.contactStarted && !result.react,gate.name);
    std::printf("%s: start=%d react=%d\n",gate.name,result.contactStarted,result.react);
    }
  if(failures!=0)
    return EXIT_FAILURE;
  std::puts("bump contract passed");
  }
