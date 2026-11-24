#pragma once

#include "baseidentity.h"

namespace zenkit { class Vfs; enum class LogLevel : std::uint8_t; }

namespace BaseImport {
// Forward the application's ZenKit log callback here. Only active import reads
// consume errors; normal archive loading keeps its existing behavior.
void onZenkitLog(zenkit::LogLevel level, const char* component, const char* message);

int convertWorld(const std::filesystem::path& gothic, const std::string& name, const std::filesystem::path& out);
int verifyWorld(const std::filesystem::path& gothic, const std::string& name, const std::filesystem::path& out,
                const std::filesystem::path& overrides = {});
int convertZen(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
int convertTextures(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
int convertMeshes(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
int convertModels(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
// P11: fonts/fonts.json and one record per selected FNT basename (common/fontdata.h),
// selected through the engine's runtime VFS (mountRuntime).
int convertFonts(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
// P12: programs/programs.json and one record per distinct program (common/programdata.h) for every
// .DAT of the runtime VFS, below the nine _compiled directories and the server's GOTHIC.DAT.
// An installation without programs gives an empty index.
int convertPrograms(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
// D2: definitions/<program SHA>/items.json, verified against fresh constructor executions.
int convertItemDefinitions(const std::filesystem::path& base);
void convertMenuDefinitions(const std::filesystem::path& base);
void convertFightDefinitions(const std::filesystem::path& base);
void convertCameraDefinitions(const std::filesystem::path& base);
void convertGameplayDefinitions(const std::filesystem::path& base);
// D3a: definitions/<program SHA>/routines.json, verified against fresh function executions.
int convertRoutineDefinitions(const std::filesystem::path& base);
// D3b: definitions/<program SHA>/npcs.json, verified against fresh constructor executions.
int convertNpcDefinitions(const std::filesystem::path& base);
int convertPlacementDefinitions(const std::filesystem::path& base);
// D4.1: definitions/<program SHA>/activities.json, routine states recognised as an activity template.
int convertActivityDefinitions(const std::filesystem::path& base);
// D4.h: deferred NPC helpers recognised and verified as native templates.
int convertNpcHelperDefinitions(const std::filesystem::path& base);
// P14: outputunits/outputunits.json and one record per distinct cutscene library (common/outputunits.h)
// for every .BIN/.DAT of the runtime VFS, the engine's host and language candidates and the server's
// and storygraph's choices. Fails when the server's native message view differs from its original
// scanner. An installation without libraries gives an empty index.
int convertOutputUnits(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
// One external OU.BIN/OU.DAT to a native record at `record` (import only; prints both SHA-256).
int convertOutputUnitsFile(const std::filesystem::path& source, const std::filesystem::path& record);
// P15: audio/audio.json and one exact copy per distinct payload (common/audiodata.h) of every .WAV/.OGG
// request of the runtime VFS and its Vfs::find winner. An installation without sounds gives an empty index.
int convertAudio(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
// P16: music/music.json and the GothicKit and Dx8 records (common/musicdata.h) of every SGT/STY/DLS either
// provider selects: each music basename of the runtime VFS and its Vfs::find winner, and every music file
// directly in the six Dx8 search directories. Stops on a GothicKit string its parser cannot convert exactly
// and on a reference naming a file of another kind. An installation without music gives an empty index.
int convertMusic(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);
// P10: sources/sources.json, the selected inputs of the not yet migrated families
// (FNT, audio, BIK, SGT/STY/DLS, the eight DATs, OU, server readers) with origin and SHA-256.
int convertSources(const std::filesystem::path& gothic, const std::filesystem::path& out, bool verifyOnly);

// Resources::loadVdfs without a mod ini, then Resources::mountWork: the engine's VFS of the
// supported Gothic II installation. Returns the mounted archives in mount order and sets
// `work` to the game's _work path (mounted with NONE only when it is a directory).
std::vector<RuntimeArchive> mountRuntime(zenkit::Vfs& vfs, const std::filesystem::path& gothic,
                                         std::filesystem::path& work);
void build(const std::filesystem::path& gothic, const std::filesystem::path& out, const Progress& progress = {});
}
