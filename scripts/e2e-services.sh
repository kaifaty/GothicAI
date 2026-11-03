#!/usr/bin/env bash
# Shared scenario requirements for service startup and dependency preparation.

e2e_needs_tts() {
  case "$1" in
    tts|svm_lines|combat_barks|mercy|hero_yield|humiliate|surrender_live|live_npc|live_director|gossip|day|group|interject|programs|errand*|fight_reactions_*) return 0 ;;
    *) return 1 ;;
  esac
}

e2e_needs_asr() {
  case "$1" in
    m3|m5e) return 0 ;;
    *) return 1 ;;
  esac
}
