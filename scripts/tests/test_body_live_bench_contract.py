"""Bench telemetry boundaries that keep filming separate from body control and replay."""
from pathlib import Path


SOURCE = (Path(__file__).resolve().parents[2] / "OpenGothic/common/testbench.cpp").read_text()


def section(start: str, end: str) -> str:
    return SOURCE.split(start, 1)[1].split(end, 1)[0]


def test_live_sink_is_opt_in_and_cannot_replace_replay_or_arena_sink():
    command = section('else if(c=="bodylive") {', 'else if(c=="bodyreplay") {')
    assert '!BodyArena::enabled() && !BodyReplay::active()' in command
    assert command.index('a[1]=="on"') < command.index('BodyController::setSink(')
    assert 'event("body_live_control","\\\"control\\\":"+std::string(row))' in command
    # Capturing the journal must never acquire a lease or alter the engine subscription.
    for operation in ('handleJson(', 'setActors(', 'reset(', 'updateWatch(', 'fixturePrepare('):
        assert operation not in command
    arena = section('void setupArena(', 'bool aiCommand(')
    assert arena.index('if(bench.bodyLive)') < arena.index('BodyArena::setup(')
    replay = section('else if(c=="bodyreplay") {', 'else if(c=="bodyobserve"')
    assert replay.index('if(bench.bodyLive)') < replay.index('BodyReplay::load(')


def test_live_sink_is_removed_at_finish_and_reconfigure_only_if_owned():
    stop = section('void stopBodyLive() {', 'const char* animationModeName()')
    assert stop.index('if(!bench.bodyLive)') < stop.index('BodyController::setSink({})')
    assert 'stopBodyLive();' in section('void finish() {', 'const char*')
    configure = section('void TestBench::configure(', 'void TestBench::holdBefore(')
    assert configure.index('stopBodyLive();') < configure.index('bench.~Bench();')


def test_frame_samples_use_same_entities_as_actions_even_when_head_is_offscreen():
    actor = section('void trackActorFrame(', 'void trackFrame(')
    assert actor.index('event("body_live_sample"') < actor.index('if(cam==nullptr)')
    assert actor.index('event("body_live_sample"') < actor.index('if(sx[main]<0)')
    assert 'BodyObservations::entityId(*n)' in actor
    assert 'world.tickCount()' in actor and 'world.player()' in actor
    assert 'n->attribute(ATR_HITPOINTS)' in actor and 'target->attribute(ATR_HITPOINTS)' in actor
    # Each actor retains the original single-NPC track schema and its own name.
    assert 'jsonStr(symbol).c_str()' in actor and 'event("track",buf)' in actor
    frame = section('void trackFrame(', '// sight:')
    assert 'for(const auto& symbol:bench.trackNpcs)' in frame
    assert 'trackActorFrame(*world,symbol,w,h)' in frame
    command = section('else if(c=="track") {', 'else if(c=="sight") {')
    assert "find(',',begin)" in command and 'bench.trackNpcs.clear()' in command
    assert 'bench.trackNpcs.end(),symbol)==bench.trackNpcs.end()' in command


def test_readiness_wait_is_asynchronous_and_times_out_explicitly():
    waiting = section('bool waitingForBodyLive() {', 'void run(')
    assert 'std::filesystem::is_regular_file(' in waiting
    assert 'std::chrono::steady_clock::now()<bench.bodyLiveDeadline' in waiting
    assert 'event("body_live_ready"' in waiting and 'event("body_live_error"' in waiting
    assert 'bench.pc=bench.prog.size();' in waiting
    assert 'sleep' not in waiting
    run = section('void run(', 'void drawLeg(')
    assert 'waitingForAi() || waitingForBodyLive()' in run
    assert '!bench.bodyLiveWait.empty() ||' in run
    event = section('void event(std::string_view kind', 'void stopBodyLive()')
    # Frames outside rec are preserved for comparison with pilot end decisions.
    assert 'bench.rec ? (long long)bench.recFrame : -1LL' in event
    assert 'std::fflush(bench.events)' in event
