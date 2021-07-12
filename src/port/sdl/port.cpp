#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "port.h"
#include "r3000a.h"
#include "plugins.h"
#include "plugin_lib.h"
#include "perfmon.h"
#include "cheat.h"
#include <SDL.h>

#if defined(DINGUX_BETA)
#include <stdlib.h>
#endif

/* PATH_MAX inclusion */
#ifdef __MINGW32__
#include <limits.h>
#include <gpu/gpulib/gpu.h>
#endif

#ifdef SPU_PCSXREARMED
#include "spu/spu_pcsxrearmed/spu_config.h"		// To set spu-specific configuration
#endif

// New gpulib from Notaz's PCSX Rearmed handles duties common to GPU plugins
#ifdef USE_GPULIB
#include "gpu/gpulib/gpu.h"
#endif

#ifdef GPU_UNAI
#include "gpu/gpu_unai/gpu.h"
#endif

#ifdef RUMBLE
#include "libShake/include/shake.h"

/* Weak rumble is either off or on */
#define RUMBLE_WEAK_MAGNITUDE SHAKE_RUMBLE_WEAK_MAGNITUDE_MAX

/* Strong rumble is internally in the range [0,255] */
#define RUMBLE_STRONG_MAGNITUDE_FACTOR (SHAKE_RUMBLE_STRONG_MAGNITUDE_MAX / 255)

typedef struct
{
	Shake_Device *device;
	Shake_Effect effect;
	int id;
	uint8_t low;
	uint8_t high;
	bool active;
	bool initialised;
} joypad_rumble_t;

static joypad_rumble_t joypad_rumble = {0};
#endif

enum {
	DKEY_SELECT = 0,
	DKEY_L3,
	DKEY_R3,
	DKEY_START,
	DKEY_UP,
	DKEY_RIGHT,
	DKEY_DOWN,
	DKEY_LEFT,
	DKEY_L2,
	DKEY_R2,
	DKEY_L1,
	DKEY_R1,
	DKEY_TRIANGLE,
	DKEY_CIRCLE,
	DKEY_CROSS,
	DKEY_SQUARE,

	DKEY_TOTAL
};

/* Hack to enable compilation with official
 * (old) GCW0 toolchain */
#if defined(GCW_ZERO) && defined(USE_BGR15)
#ifndef SDL_SWIZZLEBGR
#define SDL_SWIZZLEBGR	0x00000040
#endif
#endif

static SDL_Surface *screen;
unsigned short *SCREEN;
int SCREEN_WIDTH = 640, SCREEN_HEIGHT = 480;

static bool pcsx4all_initted = false;
static bool emu_running = false;

void update_window_size(int w, int h, bool ntsc_fix);

#ifdef GCW_ZERO

#define DINGUX_KEEP_ASPECT_FILE           "/sys/devices/platform/jz-lcd.0/keep_aspect_ratio"
#define DINGUX_SHARPNESS_UPSCALING_FILE   "/sys/devices/platform/jz-lcd.0/sharpness_upscaling"
#define DINGUX_SHARPNESS_DOWNSCALING_FILE "/sys/devices/platform/jz-lcd.0/sharpness_downscaling"

#define DINGUX_SCALING_MODE_ENVAR         "SDL_VIDEO_KMSDRM_SCALING_MODE"
#define DINGUX_SCALING_SHARPNESS_ENVAR    "SDL_VIDEO_KMSDRM_SCALING_SHARPNESS"

void set_ipu_keep_aspect_ratio(bool n) {
#if defined(DINGUX_BETA)
	setenv(DINGUX_SCALING_MODE_ENVAR, n ? "1" : "0", 1);
#else
	FILE *keep_aspect_file = fopen(DINGUX_KEEP_ASPECT_FILE, "wb");
	if (keep_aspect_file) {
		fputs(n ? "1" : "0", keep_aspect_file);
		fclose(keep_aspect_file);
	}
#endif
}

void set_ipu_filter_type(enum ipu_filter_type filter_type)
{
	/* Sharpness settings range is [0,32]
	 * - 0:      nearest-neighbour
	 * - 1:      bilinear
	 * - 2...32: bicubic (translating to a sharpness
	 *                    factor of -0.25..-4.0 internally)
	 * Default bicubic sharpness factor is
	 * (-0.125 * 8) = -1.0 */
	const char *sharpness_str        = "8";
#if !defined(DINGUX_BETA)
	FILE *sharpness_upscaling_file   = NULL;
	FILE *sharpness_downscaling_file = NULL;
#endif

	/* Check filter type */
	switch (filter_type)	{
	case IPU_FILTER_BILINEAR:
		sharpness_str = "1";
		break;
	case IPU_FILTER_NEAREST:
		sharpness_str = "0";
		break;
	case IPU_FILTER_BICUBIC:
	default:
		/* sharpness_str is already set to 8
		 * by default */
		break;
	}

#if defined(DINGUX_BETA)
	setenv(DINGUX_SCALING_SHARPNESS_ENVAR, sharpness_str, 1);
#else
	/* Set upscaling sharpness */
	sharpness_upscaling_file = fopen(DINGUX_SHARPNESS_UPSCALING_FILE, "wb");
	if (sharpness_upscaling_file) {
		fputs(sharpness_str, sharpness_upscaling_file);
		fclose(sharpness_upscaling_file);
	}

	/* Set downscaling sharpness */
	sharpness_downscaling_file = fopen(DINGUX_SHARPNESS_DOWNSCALING_FILE, "wb");
	if (sharpness_downscaling_file) {
		fputs(sharpness_str, sharpness_downscaling_file);
		fclose(sharpness_downscaling_file);
	}
#endif
}

void reset_ipu(void)
{
#if defined(DINGUX_BETA)
	unsetenv(DINGUX_SCALING_MODE_ENVAR);
	unsetenv(DINGUX_SCALING_SHARPNESS_ENVAR);
#else
	set_ipu_keep_aspect_ratio(true);
	set_ipu_filter_type(IPU_FILTER_BICUBIC);
#endif
}

#endif

static char *home                 = NULL;
static char homedir[PATH_MAX]     = "./.pcsx4all";
static char memcardsdir[PATH_MAX] = "./.pcsx4all/memcards";
static char biosdir[PATH_MAX]     = "./.pcsx4all/bios";
static char patchesdir[PATH_MAX]  = "./.pcsx4all/patches";
char sstatesdir[PATH_MAX]         = "./.pcsx4all/sstates";
char cheatsdir[PATH_MAX]          = "./.pcsx4all/cheats";
static char configdir[PATH_MAX]   = "./.pcsx4all/config";

static char McdPath1[MAXPATHLEN]  = "";
static char McdPath2[MAXPATHLEN]  = "";
static char BiosFile[MAXPATHLEN]  = "";

char CdromName[PATH_MAX]    = "";
int config_override_enabled = 0;
int config_override_active  = 0;

#ifdef __WIN32__
	#define MKDIR(A) mkdir(A)
#else
	#define MKDIR(A) mkdir(A, 0777)
#endif

static char *path_find_last_slash(const char *str)
{
	const char *slash     = strrchr(str, '/');
#ifdef __WIN32__
	const char *backslash = strrchr(str, '\\');

	if (!slash || (backslash > slash))
		return (char*)backslash;
#endif
	return (char*)slash;
}

static const char *path_basename(const char *path)
{
	const char *last = path_find_last_slash(path);
	if (last)
		return last + 1;

	return path;
}

static char *path_remove_extension(char *path)
{
	char *last = (path && *path) ? (char*)strrchr(path_basename(path), '.') : NULL;
	if (!last)
		return NULL;
	if (*last)
		*last = '\0';
	return path;
}

int path_file_exists(const char *path)
{
	int exists = 0;

	if (!string_is_empty(path)) {
#ifdef __WIN32__
		/* I don't have a Windows install for testing,
		 * so just use generic 'fopen' method
		 * (works on all platforms) */
		FILE *file = fopen(path, "r");
		if (file != NULL) {
			fclose(file);
			exists = 1;
		}
#else
		struct stat buf = {0};
		int ret = stat(path, &buf);
		if (ret == 0) {
			exists = 1;
		}
#endif
	}

	return exists;
}

void set_cdrom_name(const char *filepath)
{
	CdromName[0] = '\0';

	if (!string_is_empty(filepath)) {
		const char *basename = path_basename(filepath);
		if (!string_is_empty(basename)) {
			strcpy(CdromName, basename);
			path_remove_extension(CdromName);
		}
	}
}

static void pcsx4all_exit(void)
{
	// unload cheats
	cheat_unload();

	// Store config to file
	if (config_override_active) {
		config_save(CdromName);
	} else {
		config_save(NULL);
	}

#ifdef GCW_ZERO
	/* It is good manners to leave IPU scaling in
	 * the default state when quitting an application */
#if defined(DINGUX_BETA)
	reset_ipu();
#else
	if (!Config.VideoHwKeepAspect) {
		set_ipu_keep_aspect_ratio(true);
	}
	if ((enum ipu_filter_type)Config.VideoHwFilter != IPU_FILTER_BICUBIC) {
		set_ipu_filter_type(IPU_FILTER_BICUBIC);
	}
#endif
#endif

	if (SDL_MUSTLOCK(screen))
		SDL_UnlockSurface(screen);

	SDL_Quit();

#ifdef RUMBLE
	if (joypad_rumble.device)
	{
		if (joypad_rumble.active)
			Shake_Stop(joypad_rumble.device, joypad_rumble.id);

		Shake_EraseEffect(joypad_rumble.device, joypad_rumble.id);

		Shake_Close(joypad_rumble.device);
		memset(&joypad_rumble, 0, sizeof(joypad_rumble_t));
	}
	Shake_Quit();
#endif

	if (pcsx4all_initted == true) {
		ReleasePlugins();
		psxShutdown();
	}
}

static void setup_paths()
{
#ifndef __WIN32__
	home = getenv("HOME");
#else
	char buf[PATH_MAX];
	home = getcwd(buf, PATH_MAX);
#endif
	if (home) {
		sprintf(homedir,     "%s/.pcsx4all", home);
		sprintf(sstatesdir,  "%s/sstates", homedir);
		sprintf(memcardsdir, "%s/memcards", homedir);
		sprintf(biosdir,     "%s/bios", homedir);
		sprintf(patchesdir,  "%s/patches", homedir);
		sprintf(cheatsdir,   "%s/cheats", homedir);
		sprintf(configdir,   "%s/config", homedir);
	}

	MKDIR(homedir);
	MKDIR(sstatesdir);
	MKDIR(memcardsdir);
	MKDIR(biosdir);
	MKDIR(patchesdir);
	MKDIR(cheatsdir);
	MKDIR(configdir);
}

void probe_lastdir()
{
	DIR *dir;
	if (!Config.LastDir)
		return;

	dir = opendir(Config.LastDir);

	if (!dir) {
		// Fallback to home directory.
		strncpy(Config.LastDir, home, MAXPATHLEN);
		Config.LastDir[MAXPATHLEN-1] = '\0';
	} else {
		closedir(dir);
	}
}

#ifdef PSXREC
extern u32 cycle_multiplier; // in mips/recompiler.cpp
#endif

void config_get_override_filename(const char *diskname, char *filename)
{
	*filename = '\0';
	if (!string_is_empty(diskname)) {
		sprintf(filename, "%s/%s.cfg", configdir, diskname);
	}
}

int config_load(const char *diskname)
{
	FILE *f     = NULL;
	int lineNum = 0;
	char config[MAXPATHLEN];
	char line[MAXPATHLEN + 8 + 1];

	line[0]   = '\0';

	/* If a disk name is provided, attempt to open
	 * config override */
	if (!string_is_empty(diskname)) {
		config_get_override_filename(diskname, config);
		f = fopen(config, "r");
		if (f == NULL) {
			printf("Failed to open config override: \"%s\" for reading - using default config.\n", config);
		}
	}

	/* Otherwise attempt to open default config file */
	if (f == NULL) {
		config[0] = '\0';
		sprintf(config, "%s/pcsx4all_ng.cfg", homedir);
		f = fopen(config, "r");
	}

	/* Otherwise attempt to open legacy config file */
	if (f == NULL) {
		config[0] = '\0';
		sprintf(config, "%s/pcsx4all.cfg", homedir);
		f = fopen(config, "r");
		if (f == NULL) {
			printf("Failed to open config file: \"%s\" for reading.\n", config);
			return 0;
		}
	}

	while (fgets(line, sizeof(line), f)) {
		char *arg = strchr(line, ' ');
		int value;

		++lineNum;

		if (!arg) {
			continue;
		}

		*arg = '\0';
		arg++;

		if (lineNum == 1) {
			if (!strcmp(line, "CONFIG_VERSION")) {
				sscanf(arg, "%d", &value);
				if (value == CONFIG_VERSION) {
					continue;
				} else {
					printf("Incompatible config version for \"%s\"."
					       "Required: %d. Found: %d. Ignoring.\n",
					       config, CONFIG_VERSION, value);
					break;
				}
			}

			printf("Incompatible config format for \"%s\"."
			       "Ignoring.\n", config);
			break;
		}

		if (!strcmp(line, "Xa")) {
			sscanf(arg, "%d", &value);
			Config.Xa = value;
		} else if (!strcmp(line, "Mdec")) {
			sscanf(arg, "%d", &value);
			Config.Mdec = value;
		} else if (!strcmp(line, "PsxAuto")) {
			sscanf(arg, "%d", &value);
			Config.PsxAuto = value;
		} else if (!strcmp(line, "Cdda")) {
			sscanf(arg, "%d", &value);
			Config.Cdda = value;
		} else if (!strcmp(line, "AsyncCD")) {
			sscanf(arg, "%d", &value);
			Config.AsyncCD = value;
		} else if (!strcmp(line, "HLE")) {
			sscanf(arg, "%d", &value);
			Config.HLE = value;
		} else if (!strcmp(line, "SlowBoot")) {
			sscanf(arg, "%d", &value);
			Config.SlowBoot = value;
		} else if (!strcmp(line, "AnalogArrow")) {
			sscanf(arg, "%d", &value);
			Config.AnalogArrow = value;
		} else if (!strcmp(line, "Analog_Mode")) {
			sscanf(arg, "%d", &value);
			if (value < 0 || value > 3)
				value = 3;
			Config.AnalogMode = value;
		} else if (!strcmp(line, "RumbleGain")) {
			sscanf(arg, "%d", &value);
			if (value < 0 || value > 100)
				value = 100;
			Config.RumbleGain = value;
		} else if (!strcmp(line, "MenuToggleCombo")) {
			sscanf(arg, "%d", &value);
			Config.MenuToggleCombo = value;
		} else if (!strcmp(line, "RCntFix")) {
			sscanf(arg, "%d", &value);
			Config.RCntFix = value;
		} else if (!strcmp(line, "VSyncWA")) {
			sscanf(arg, "%d", &value);
			Config.VSyncWA = value;
		} else if (!strcmp(line, "Cpu")) {
			sscanf(arg, "%d", &value);
			Config.Cpu = value;
		} else if (!strcmp(line, "PsxType")) {
			sscanf(arg, "%d", &value);
			Config.PsxType = value;
		} else if (!strcmp(line, "McdSlots")) {
			sscanf(arg, "%d", &value);
			Config.McdSlots = value;
		} else if (!strcmp(line, "McdSlot1")) {
			sscanf(arg, "%d", &value);
			Config.McdSlot1 = value;
		} else if (!strcmp(line, "McdSlot2")) {
			sscanf(arg, "%d", &value);
			Config.McdSlot2 = value;
		} else if (!strcmp(line, "SpuIrq")) {
			sscanf(arg, "%d", &value);
			Config.SpuIrq = value;
		} else if (!strcmp(line, "SyncAudio")) {
			sscanf(arg, "%d", &value);
			Config.SyncAudio = value;
		} else if (!strcmp(line, "SpuUpdateFreq")) {
			sscanf(arg, "%d", &value);
			if (value < SPU_UPDATE_FREQ_MIN || value > SPU_UPDATE_FREQ_MAX)
				value = SPU_UPDATE_FREQ_DEFAULT;
			Config.SpuUpdateFreq = value;
		} else if (!strcmp(line, "ForcedXAUpdates")) {
			sscanf(arg, "%d", &value);
			if (value < FORCED_XA_UPDATES_MIN || value > FORCED_XA_UPDATES_MAX)
				value = FORCED_XA_UPDATES_DEFAULT;
			Config.ForcedXAUpdates = value;
		} else if (!strcmp(line, "ShowFps")) {
			sscanf(arg, "%d", &value);
			Config.ShowFps = value;
		} else if (!strcmp(line, "FrameLimit")) {
			sscanf(arg, "%d", &value);
			Config.FrameLimit = value;
		} else if (!strcmp(line, "FrameSkip")) {
			sscanf(arg, "%d", &value);
			if (value < FRAMESKIP_MIN || value > FRAMESKIP_MAX)
				value = FRAMESKIP_OFF;
			Config.FrameSkip = value;
		} else if (!strcmp(line, "VideoScaling")) {
			sscanf(arg, "%d", &value);
			Config.VideoScaling = value;
		} else if (!strcmp(line, "VideoHwKeepAspect")) {
			sscanf(arg, "%d", &value);
			Config.VideoHwKeepAspect = value;
		} else if (!strcmp(line, "VideoHwFilter")) {
			sscanf(arg, "%d", &value);
			Config.VideoHwFilter = value;
		}
#ifdef SPU_PCSXREARMED
		else if (!strcmp(line, "SpuUseInterpolation")) {
			sscanf(arg, "%d", &value);
			spu_config.iUseInterpolation = value;
		} else if (!strcmp(line, "SpuUseReverb")) {
			sscanf(arg, "%d", &value);
			spu_config.iUseReverb = value;
		} else if (!strcmp(line, "SpuVolume")) {
			sscanf(arg, "%d", &value);
			if (value > 1024) value = 1024;
			if (value < 0) value = 0;
			spu_config.iVolume = value;
		}
#endif
		else if (!strcmp(line, "LastDir")) {
			int len = strlen(arg);

			if (len == 0 || len > sizeof(Config.LastDir) - 1) {
				continue;
			}

			if (arg[len-1] == '\n') {
				arg[len-1] = '\0';
			}

			strcpy(Config.LastDir, arg);
		} else if (!strcmp(line, "BiosDir")) {
			int len = strlen(arg);

			if (len == 0 || len > sizeof(Config.BiosDir) - 1) {
				continue;
			}

			if (arg[len-1] == '\n') {
				arg[len-1] = '\0';
			}

			strcpy(Config.BiosDir, arg);
		} else if (!strcmp(line, "Bios")) {
			int len = strlen(arg);

			if (len == 0 || len > sizeof(Config.Bios) - 1) {
				continue;
			}

			if (arg[len-1] == '\n') {
				arg[len-1] = '\0';
			}

			strcpy(Config.Bios, arg);
		}
#ifdef PSXREC
		else if (!strcmp(line, "CycleMultiplier")) {
			sscanf(arg, "%03x", &value);
			cycle_multiplier = value;
		}
#endif
#ifdef GPU_UNAI
		else if (!strcmp(line, "pixel_skip")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.pixel_skip = value;
		}
		else if (!strcmp(line, "lighting")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.lighting = value;
		} else if (!strcmp(line, "fast_lighting")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.fast_lighting = value;
		} else if (!strcmp(line, "blending")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.blending = value;
		} else if (!strcmp(line, "dithering")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.dithering = value;
		} else if (!strcmp(line, "interlace")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.ilace_force = value;
		} else if (!strcmp(line, "ntsc_fix")) {
			sscanf(arg, "%d", &value);
			gpu_unai_config_ext.ntsc_fix = value;
		}
#endif
	}

	fclose(f);
	return 1;
}

int config_save(const char *diskname)
{
	FILE *f = NULL;
	char config[MAXPATHLEN];

	/* If a disk name is provided, attempt to open
	 * config override */
	if (!string_is_empty(diskname)) {
		config_get_override_filename(diskname, config);
		f = fopen(config, "w");
		if (f == NULL) {
			printf("Failed to open config override: \"%s\" for writing.\n", config);
			return 0;
		}
	}

	/* Otherwise attempt to open default config file */
	if (f == NULL) {
		config[0] = '\0';
		sprintf(config, "%s/pcsx4all_ng.cfg", homedir);
		f = fopen(config, "w");
	}

	if (f == NULL) {
		printf("Failed to open config file: \"%s\" for writing.\n", config);
		return 0;
	}

	fprintf(f, "CONFIG_VERSION %d\n"
		   "Xa %d\n"
		   "Mdec %d\n"
		   "PsxAuto %d\n"
		   "Cdda %d\n"
		   "AsyncCD %d\n"
		   "HLE %d\n"
		   "SlowBoot %d\n"
		   "AnalogArrow %d\n"
		   "Analog_Mode %d\n"
		   "RumbleGain %d\n"
		   "MenuToggleCombo %d\n"
		   "RCntFix %d\n"
		   "VSyncWA %d\n"
		   "Cpu %d\n"
		   "PsxType %d\n"
		   "McdSlots %d\n"
		   "McdSlot1 %d\n"
		   "McdSlot2 %d\n"
		   "SpuIrq %d\n"
		   "SyncAudio %d\n"
		   "SpuUpdateFreq %d\n"
		   "ForcedXAUpdates %d\n"
		   "ShowFps %d\n"
		   "FrameLimit %d\n"
		   "FrameSkip %d\n"
		   "VideoScaling %d\n"
		   "VideoHwKeepAspect %d\n"
		   "VideoHwFilter %d\n",
		   CONFIG_VERSION, Config.Xa, Config.Mdec, Config.PsxAuto, Config.Cdda, Config.AsyncCD,
		   Config.HLE, Config.SlowBoot, Config.AnalogArrow, Config.AnalogMode, Config.RumbleGain, Config.MenuToggleCombo,
		   Config.RCntFix, Config.VSyncWA, Config.Cpu, Config.PsxType,
		   Config.McdSlots, Config.McdSlot1, Config.McdSlot2, Config.SpuIrq, Config.SyncAudio,
		   Config.SpuUpdateFreq, Config.ForcedXAUpdates, Config.ShowFps,
		   Config.FrameLimit, Config.FrameSkip, Config.VideoScaling,
		   Config.VideoHwKeepAspect, Config.VideoHwFilter);

#ifdef SPU_PCSXREARMED
	fprintf(f, "SpuUseInterpolation %d\n", spu_config.iUseInterpolation);
	fprintf(f, "SpuUseReverb %d\n", spu_config.iUseReverb);
	fprintf(f, "SpuVolume %d\n", spu_config.iVolume);
#endif

#ifdef PSXREC
	fprintf(f, "CycleMultiplier %03x\n", cycle_multiplier);
#endif

#ifdef GPU_UNAI
	fprintf(f, "interlace %d\n"
		   "pixel_skip %d\n"
		   "lighting %d\n"
		   "fast_lighting %d\n"
		   "blending %d\n"
		   "dithering %d\n"
		   "ntsc_fix %d\n",
		   gpu_unai_config_ext.ilace_force,
		   gpu_unai_config_ext.pixel_skip,
		   gpu_unai_config_ext.lighting,
		   gpu_unai_config_ext.fast_lighting,
		   gpu_unai_config_ext.blending,
		   gpu_unai_config_ext.dithering,
		   gpu_unai_config_ext.ntsc_fix);
#endif

	if (Config.LastDir[0]) {
		fprintf(f, "LastDir %s\n", Config.LastDir);
	}

	if (Config.BiosDir[0]) {
		fprintf(f, "BiosDir %s\n", Config.BiosDir);
	}

	if (Config.Bios[0]) {
		fprintf(f, "Bios %s\n", Config.Bios);
	}

	fclose(f);
	return 1;
}

// Returns 0: success, -1: failure
int state_load(int slot)
{
	char savename[512];
	sprintf(savename, "%s/%s.%d.sav", sstatesdir, CdromId, slot);

	if (FileExists(savename)) {
		return LoadState(savename);
	}

	return -1;
}

// Returns 0: success, -1: failure
int state_save(int slot)
{
	char savename[512];
	sprintf(savename, "%s/%s.%d.sav", sstatesdir, CdromId, slot);

	return SaveState(savename);
}

static struct {
	int key;
	int bit;
} keymap[] = {
	{ SDLK_UP,        DKEY_UP },
	{ SDLK_DOWN,      DKEY_DOWN },
	{ SDLK_LEFT,      DKEY_LEFT },
	{ SDLK_RIGHT,     DKEY_RIGHT },
#if defined(GCW_ZERO)
	{ SDLK_LSHIFT,    DKEY_SQUARE },
	{ SDLK_LCTRL,     DKEY_CIRCLE },
	{ SDLK_SPACE,     DKEY_TRIANGLE },
	{ SDLK_LALT,      DKEY_CROSS },
	{ SDLK_TAB,       DKEY_L1 },
	{ SDLK_BACKSPACE, DKEY_R1 },
	{ SDLK_ESCAPE,    DKEY_SELECT },
#if defined(PG2)
	{ SDLK_RSHIFT,    DKEY_L2 },
	{ SDLK_RALT,      DKEY_R2 },
#elif defined(RG350)
	{ SDLK_PAGEUP,    DKEY_L2 },
	{ SDLK_PAGEDOWN,  DKEY_R2 },
#if !defined(PG2V2)
	{ SDLK_KP_DIVIDE, DKEY_L3 },
	{ SDLK_KP_PERIOD, DKEY_R3 },
#endif
#endif
#else
	{ SDLK_a,         DKEY_SQUARE },
	{ SDLK_x,         DKEY_CIRCLE },
	{ SDLK_s,         DKEY_TRIANGLE },
	{ SDLK_z,         DKEY_CROSS },
	{ SDLK_q,         DKEY_L1 },
	{ SDLK_w,         DKEY_R1 },
	{ SDLK_e,         DKEY_L2 },
	{ SDLK_r,         DKEY_R2 },
	{ SDLK_BACKSPACE, DKEY_SELECT },
#endif
	{ SDLK_RETURN,    DKEY_START },
	{ 0, 0 }
};

static uint16_t pad1 = 0xFFFF;

static uint16_t pad2 = 0xFFFF;

static uint16_t pad1_buttons = 0xFFFF;

static unsigned short analog1 = 0;

SDL_Joystick* sdl_joy[2];

#define joy_commit_range    8192
enum {
	ANALOG_UP = 1,
	ANALOG_DOWN = 2,
	ANALOG_LEFT = 4,
	ANALOG_RIGHT = 8
};

struct ps1_controller player_controller[2];

void Set_Controller_Mode()
{
	switch (Config.AnalogMode) {
		/* Digital. Required for some games. */
	default: player_controller[0].id = 0x41;
		player_controller[0].pad_mode = 0;
		player_controller[0].pad_controllertype = PSE_PAD_TYPE_STANDARD;
		break;
		/* DualAnalog. Some games might misbehave with Dualshock like Descent so this is for those */
	case 1: player_controller[0].id = 0x53;
		player_controller[0].pad_mode = 1;
		player_controller[0].pad_controllertype = PSE_PAD_TYPE_ANALOGPAD;
		break;
		/* DualShock, required for Ape Escape. */
	case 2: 
		player_controller[0].id = 0x41;
		player_controller[0].pad_mode = 0;
		player_controller[0].pad_controllertype = PSE_PAD_TYPE_ANALOGPAD;
		break;
	case 3: 
		player_controller[0].id = 0x73;
		player_controller[0].pad_mode = 1;
		player_controller[0].pad_controllertype = PSE_PAD_TYPE_ANALOGPAD;
		break;
	}
}

void joy_init()
{

	sdl_joy[0] = SDL_JoystickOpen(0);
	sdl_joy[1] = SDL_JoystickOpen(1);

	player_controller[0].joy_left_ax0 = 127;
	player_controller[0].joy_left_ax1 = 127;
	player_controller[0].joy_right_ax0 = 127;
	player_controller[0].joy_right_ax1 = 127;

	player_controller[0].Vib[0] = 0;
	player_controller[0].Vib[1] = 0;
	player_controller[0].VibF[0] = 0;
	player_controller[0].VibF[1] = 0;

	player_controller[0].id = 0x41;
	player_controller[0].pad_mode = 0;
	player_controller[0].pad_controllertype = 0;

	player_controller[0].configmode = 0;

	Set_Controller_Mode();
}

void pad_update()
{
	int axisval;
	SDL_Event event;
	Uint8 *keys = SDL_GetKeyState(NULL);
	bool popup_menu = false;

	int k = 0;
	while (keymap[k].key) {
		if (keys[keymap[k].key]) {
			pad1_buttons &= ~(1 << keymap[k].bit);
		} else {
			pad1_buttons |= (1 << keymap[k].bit);
		}
		k++;
	}

	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			exit(0);
			break;
		case SDL_KEYDOWN:
			switch (event.key.keysym.sym) {
#if defined(PG2) || defined(PG2V2)
			case SDLK_RCTRL:
#else
			case SDLK_HOME:
			case SDLK_F10:
#endif
				popup_menu = true;
				break;
#ifndef GCW_ZERO
			case SDLK_ESCAPE:
#endif
				event.type = SDL_QUIT;
				SDL_PushEvent(&event);
				break;
			case SDLK_v: { Config.ShowFps=!Config.ShowFps; } break;
				default: break;
			}
			break;
		case SDL_JOYAXISMOTION:
			switch (event.jaxis.axis) {
			case 0: /* X axis */
				axisval = event.jaxis.value;
				if (Config.AnalogArrow == 1) {
					analog1 &= ~(ANALOG_LEFT | ANALOG_RIGHT);
					if (axisval > joy_commit_range) {
						analog1 |= ANALOG_RIGHT;
					} else if (axisval < -joy_commit_range) {
						analog1 |= ANALOG_LEFT;
					}
				} else {
					player_controller[0].joy_left_ax0 = (axisval + 0x8000) >> 8;
				}
				break;
			case 1: /* Y axis */
				axisval = event.jaxis.value;
				if (Config.AnalogArrow == 1) {
					analog1 &= ~(ANALOG_UP | ANALOG_DOWN);
					if (axisval > joy_commit_range) {
						analog1 |= ANALOG_DOWN;
					} else if (axisval < -joy_commit_range) {
						analog1 |= ANALOG_UP;
					}
				} else {
					player_controller[0].joy_left_ax1 = (axisval + 0x8000) >> 8;
				}
				break;
#if !defined(PG2) && !defined(PG2V2)
			case 2: /* X axis */
				axisval = event.jaxis.value;
				player_controller[0].joy_right_ax0 = (axisval + 0x8000) >> 8;
				break;
			case 3: /* Y axis */
				axisval = event.jaxis.value;
				player_controller[0].joy_right_ax1 = (axisval + 0x8000) >> 8;
				break;
#endif
			}
			break;
#if !defined(PG2) && !defined(PG2V2)
		case SDL_JOYBUTTONDOWN:
			if (event.jbutton.which == 0) {
				pad1_buttons |= (1 << DKEY_L3);
			} else if (event.jbutton.which == 1) {
				pad1_buttons |= (1 << DKEY_R3);
			}
			break;
#endif
		default: break;
		}
	}
	
#if defined(GCW_ZERO) && !defined(RG350) && !defined(PG2)
	// SELECT+B for psx's SELECT
	if (keys[SDLK_ESCAPE] && keys[SDLK_LALT]) {
		pad1 &= ~(1 << DKEY_SELECT);
		pad1 |= (1 << DKEY_CROSS);
	} else {
		pad1 |= (1 << DKEY_SELECT);
	}

	// SELECT+L1 for psx's L2
	if (keys[SDLK_ESCAPE] && keys[SDLK_TAB]) {
		pad1 &= ~(1 << DKEY_L2);
		pad1 |= (1 << DKEY_L1);
	} else {
		pad1 |= (1 << DKEY_L2);
	}

	// SELECT+R1 for R2
	if (keys[SDLK_ESCAPE] && keys[SDLK_BACKSPACE]) {
		pad1 &= ~(1 << DKEY_R2);
		pad1 |= (1 << DKEY_R1);
	} else {
		pad1 |= (1 << DKEY_R2);
	}

	// SELECT+START for menu
	if (keys[SDLK_ESCAPE] && keys[SDLK_RETURN] && !keys[SDLK_LALT]) {
		//Sync and close any memcard files opened for writing
		//TODO: Disallow entering menu until they are synced/closed
		// automatically, displaying message that write is in progress.
		sioSyncMcds();

		emu_running = false;
		pl_pause();    // Tell plugin_lib we're pausing emu
		GameMenu();
		emu_running = true;
		pad1 |= (1 << DKEY_START);
		pad1 |= (1 << DKEY_CROSS);
		video_clear();
		video_flip();
		video_clear();
#ifdef SDL_TRIPLEBUF
		video_flip();
		video_clear();
#endif
		pl_resume();    // Tell plugin_lib we're reentering emu
	}
#endif
	
	if (Config.AnalogArrow == 1) {
		if ((pad1_buttons & (1 << DKEY_UP)) && (analog1 & ANALOG_UP)) {
			pad1_buttons &= ~(1 << DKEY_UP);
		}
		if ((pad1_buttons & (1 << DKEY_DOWN)) && (analog1 & ANALOG_DOWN)) {
			pad1_buttons &= ~(1 << DKEY_DOWN);
		}
		if ((pad1_buttons & (1 << DKEY_LEFT)) && (analog1 & ANALOG_LEFT)) {
			pad1_buttons &= ~(1 << DKEY_LEFT);
		}
		if ((pad1_buttons & (1 << DKEY_RIGHT)) && (analog1 & ANALOG_RIGHT)) {
			pad1_buttons &= ~(1 << DKEY_RIGHT);
		}
	}

	/* Handle menu toggle button combinations */
	switch (Config.MenuToggleCombo) {
	case 0: /* L3 + R3 */
		if (!(pad1_buttons & ((1 << DKEY_L3) | (1 << DKEY_R3)))) {
			popup_menu = true;
		}
		break;
	case 1: /* SELECT + START */
		if (!(pad1_buttons & ((1 << DKEY_SELECT) | (1 << DKEY_START)))) {
			popup_menu = true;
		}
		break;
	default:
		break;
	}

	// popup main menu
	if (popup_menu) {
		//Sync and close any memcard files opened for writing
		//TODO: Disallow entering menu until they are synced/closed
		// automatically, displaying message that write is in progress.
		sioSyncMcds();

		emu_running = false;
		pl_pause();    // Tell plugin_lib we're pausing emu
		update_window_size(320, 240, false);

		/* Disable any rumble effects */
		trigger_rumble(0, 0);

		GameMenu();
		emu_running = true;
		pad1_buttons |= (1 << DKEY_SELECT) | (1 << DKEY_START) | (1 << DKEY_CROSS);
		update_window_size(gpu.screen.hres, gpu.screen.vres, Config.PsxType == PSX_TYPE_NTSC);
		if (Config.VideoScaling == 1) {
			video_clear_cache();
		}
		emu_running = true;
		pad1 |= (1 << DKEY_START);
		pad1 |= (1 << DKEY_CROSS);
		pl_resume();    // Tell plugin_lib we're reentering emu
	}

	pad1 = pad1_buttons;
}

unsigned short pad_read(int num)
{
	return (num == 0 ? pad1 : pad2);
}

void video_flip(void)
{
	if (emu_running && Config.ShowFps) {
		port_printf(5, 5, pl_data.stats_msg);
	}

	if (SDL_MUSTLOCK(screen))
		SDL_UnlockSurface(screen);

	SDL_Flip(screen);

	if (SDL_MUSTLOCK(screen))
		SDL_LockSurface(screen);

	SCREEN = (Uint16 *)screen->pixels;
}

/* This is used by gpu_dfxvideo only as it doesn't scale itself */
#ifdef GPU_DFXVIDEO
void video_set(unsigned short *pVideo, unsigned int width, unsigned int height)
{
	int y;
	unsigned short *ptr = SCREEN;
	int w = (width > 320 ? 320 : width);
	int h = (height > 240 ? 240 : height);

	for (y = 0; y < h; y++) {
		memcpy(ptr, pVideo, w * 2);
		ptr += 320;
		pVideo += width;
	}

	video_flip();
}
#endif

void video_clear(void)
{
	memset(screen->pixels, 0, screen->pitch*screen->h);
}

void video_clear_cache()
{
	video_clear();
	video_flip();
	video_clear();
#ifdef SDL_TRIPLEBUF
	video_flip();
	video_clear();
#endif
}

const char *GetMemcardPath(int slot) {
	switch(slot) {
	case 1:
		return McdPath1;
	case 2:
		return McdPath2;
	}
	return NULL;
}

void update_memcards(int load_mcd) {

		if (Config.McdSlot1 == 17) {
			if (string_is_empty(CdromId)) {
				/* Fallback */
				sprintf(McdPath1, "%s/%s", memcardsdir, "card1.mcd");
			} else {
				sprintf(McdPath1, "%s/%s.1.mcr", memcardsdir, CdromId);
			}
		} else {
			sprintf(McdPath1, "%s/mcd%03d.mcr", memcardsdir, (int)Config.McdSlot1);
			}
		if (Config.McdSlots == 2 || Config.McdSlots == 3) {
			sprintf(McdPath1, "none", 0);
		} 

		if (Config.McdSlot2 == 17) {
			if (string_is_empty(CdromId)) {
				/* Fallback */
				sprintf(McdPath2, "%s/%s", memcardsdir, "card2.mcd");
			} else {
				sprintf(McdPath2, "%s/%s.2.mcr", memcardsdir, CdromId);
			}
		} else {
			sprintf(McdPath2, "%s/mcd%03d.mcr", memcardsdir, (int)Config.McdSlot2);
		}
		if (Config.McdSlots == 1 || Config.McdSlots == 3) {
			sprintf(McdPath2, "none", 0);
		}

	if (load_mcd & 1)
		LoadMcd(MCD1, McdPath1); //Memcard 1
	if (load_mcd & 2)
		LoadMcd(MCD2, McdPath2); //Memcard 2
}

const char *bios_file_get() {
	return BiosFile;
}

void bios_file_set(const char *filename) {
	strcpy(Config.Bios, filename);
	strcpy(BiosFile, filename);
}

// if [CdromId].bin is exsit, use the spec bios
void check_spec_bios() {
	FILE *f = NULL;
	char bios[MAXPATHLEN];
	sprintf(bios, "%s/%s.bin", Config.BiosDir, CdromId);
	f = fopen(bios, "rb");
	if (f == NULL) {
		strcpy(BiosFile, Config.Bios);
		return;
	}
	fclose(f);
	sprintf(BiosFile, "%s.bin", CdromId);
}


/* This is needed to override redirecting to stderr.txt and stdout.txt
with mingw build. */
#ifdef UNDEF_MAIN
#undef main
#endif

#ifdef RUMBLE
int set_rumble_gain(unsigned gain)
{
	if (!joypad_rumble.device) {
		return 0;
	}

	if (Shake_SetGain(joypad_rumble.device, (int)gain) != SHAKE_OK) {
		return 0;
	}

	return 1;
}

void Rumble_Init() {
	bool effect_uploaded = false;

	if (joypad_rumble.initialised)
	{
		printf("ERROR: Rumble already initialized !\n");
		return;
	}

	memset(&joypad_rumble, 0, sizeof(joypad_rumble_t));
	Shake_Init();

	/* If gain is zero, no need to initialise device */
	if (Config.RumbleGain == 0)
		goto error;

	if (Shake_NumOfDevices() < 1)
		goto error;

	/* Open shake device */
	joypad_rumble.device = Shake_Open(0);

	if (!joypad_rumble.device)
		goto error;

	/* Check whether shake device has the
	 * required feature set */
	if (!Shake_QueryEffectSupport(joypad_rumble.device, SHAKE_EFFECT_RUMBLE) ||
		 !Shake_QueryGainSupport(joypad_rumble.device))
		goto error;

	/* Initialise rumble effect */
	if (Shake_InitEffect(&joypad_rumble.effect, SHAKE_EFFECT_RUMBLE) != SHAKE_OK)
		goto error;

	joypad_rumble.effect.u.rumble.weakMagnitude   = 0;
	joypad_rumble.effect.u.rumble.strongMagnitude = 0;
	joypad_rumble.effect.length                   = 0; /* Infinite */
	joypad_rumble.effect.delay                    = 0;
	joypad_rumble.id                              = Shake_UploadEffect(
			joypad_rumble.device, &joypad_rumble.effect);

	if (joypad_rumble.id == SHAKE_ERROR)
		goto error;
	effect_uploaded = true;

	/* Set gain */
	if (!set_rumble_gain(Config.RumbleGain))
		goto error;

	printf("Rumble initialized !\n");
	joypad_rumble.initialised = true;
	return;

error:
	printf("Rumble effects disabled...\n");
	joypad_rumble.initialised = true;

	if (joypad_rumble.device)
	{
		if (effect_uploaded)
			Shake_EraseEffect(joypad_rumble.device, joypad_rumble.id);

		Shake_Close(joypad_rumble.device);
		joypad_rumble.device = NULL;
	}
}

int trigger_rumble(uint8_t low, uint8_t high)
{
	if (!joypad_rumble.device) {
		return 0;
	}

	/* If total strength is zero, halt rumble effect */
	if ((low == 0) && (high == 0)) {
		if (joypad_rumble.active) {
			if (Shake_Stop(joypad_rumble.device, joypad_rumble.id) == SHAKE_OK) {
				joypad_rumble.active = false;
				return 1;
			} else {
				return 0;
			}
		}

		return 1;
	}

	/* If strength has changed, update effect */
	if ((low != joypad_rumble.low) || (high != joypad_rumble.high)) {
		int id;

		/* Workaround for an inexplicable bug:
		 * - We are supposed to be able to change rumble
		 *   strength dynamically, without stopping a
		 *   currently running effect
		 * - This works in most cases, but:
		 * - If the effect is currently running with
		 *   *both* low and high at a non-zero value and
		 *   we change one of them to a zero value (or vice
		 *   versa), then the next Shake_Stop() will fail
		 *   without error, causing the rumble effect to
		 *   continue indefinitely
		 * - We therefore have to manually stop the running
		 *   effect in this case, before uploading the new
		 *   settings... :( */
		if (joypad_rumble.active && ((low && high) != (joypad_rumble.low && joypad_rumble.high))) {
			if (Shake_Stop(joypad_rumble.device, joypad_rumble.id) == SHAKE_OK) {
				joypad_rumble.active = false;
			} else {
				return 0;
			}
		}

		joypad_rumble.effect.id                       = joypad_rumble.id;
		joypad_rumble.effect.u.rumble.weakMagnitude   = low ? RUMBLE_WEAK_MAGNITUDE : 0x0;
		joypad_rumble.effect.u.rumble.strongMagnitude = (uint16_t)high * RUMBLE_STRONG_MAGNITUDE_FACTOR;
		id                                            = Shake_UploadEffect(
				joypad_rumble.device, &joypad_rumble.effect);

		if (id == SHAKE_ERROR) {
			return 0;
		}

		joypad_rumble.id                              = id;
		joypad_rumble.low                             = low;
		joypad_rumble.high                            = high;
	}

	/* If effect is currently idle, activate it */
	if (!joypad_rumble.active) {
		if (Shake_Play(joypad_rumble.device, joypad_rumble.id) == SHAKE_OK) {
			joypad_rumble.active = true;
			return 1;
		} else {
			return 0;
		}
	}

	return 1;
}
#else
void Rumble_Init() {}
void trigger_rumble(uint8_t low, uint8_t high) {}
#endif

void update_window_size(int w, int h, bool ntsc_fix)
{
	if (Config.VideoScaling != 0) return;
#ifdef SDL_TRIPLEBUF
	int flags = SDL_TRIPLEBUF;
#else
	int flags = SDL_DOUBLEBUF;
#endif
    flags |= SDL_HWSURFACE
#if defined(GCW_ZERO) && defined(USE_BGR15)
        | SDL_SWIZZLEBGR
#endif
        ;
	SCREEN_WIDTH = w;
	if (gpu_unai_config_ext.ntsc_fix && ntsc_fix) {
		switch (h) {
		case 240:
		case 256: h -= 16; break;
		case 480: h -= 32; break;
		}
	}
	SCREEN_HEIGHT = h;

	if (screen && SDL_MUSTLOCK(screen))
		SDL_UnlockSurface(screen);

	screen = SDL_SetVideoMode(SCREEN_WIDTH, SCREEN_HEIGHT,
#if !defined(GCW_ZERO) || !defined(USE_BGR15)
			16,
#else
			15,
#endif
			flags);
	if (!screen) {
		puts("SDL_SetVideoMode error");
		exit(0);
	}

	if (SDL_MUSTLOCK(screen))
		SDL_LockSurface(screen);

	SCREEN = (Uint16 *)screen->pixels;

#if !defined(GCW_ZERO) && defined(USE_BGR15)
	screen->format->Rshift = 0;
	screen->format->Gshift = 5;
	screen->format->Bshift = 10;
	screen->format->Rmask = 0x1Fu;
	screen->format->Gmask = 0x1Fu<<5u;
	screen->format->Bmask = 0x1Fu<<10u;
	screen->format->Amask = 0;
	screen->format->Ashift = 0;
	screen->format_version++;
#endif

	video_clear_cache();
}

int main (int argc, char **argv)
{
	char filename[256];
	const char *cdrfilename = GetIsoFile();

	filename[0] = '\0'; /* Executable file name */

	setup_paths();

	// PCSX
	Config.McdSlot1 = 1;
	Config.McdSlot2 = 2;
	update_memcards(0);
	strcpy(Config.PatchesDir, patchesdir);
	strcpy(Config.BiosDir, biosdir);
	strcpy(Config.Bios, "scph1001.bin");

	Config.Xa=0; /* 0=XA enabled, 1=XA disabled */
	Config.Mdec=0; /* 0=Black&White Mdecs Only Disabled, 1=Black&White Mdecs Only Enabled */
	Config.PsxAuto=1; /* 1=autodetect system (pal or ntsc) */
	Config.PsxType=0; /* PSX_TYPE_NTSC=ntsc, PSX_TYPE_PAL=pal */
	Config.Cdda=0; /* 0=Enable Cd audio, 1=Disable Cd audio */
	Config.AsyncCD=0; /* 0=Synchronous Cd access, 1=Asynchronous Cd access */
	Config.HLE=0; /* 0=BIOS, 1=HLE */
#if defined (PSXREC)
	Config.Cpu=0; /* 0=recompiler, 1=interpreter */
#else
	Config.Cpu=1; /* 0=recompiler, 1=interpreter */
#endif
	Config.SlowBoot=0; /* 0=skip bios logo sequence on boot  1=show sequence (does not apply to HLE) */
	Config.RCntFix=0; /* 1=Parasite Eve 2, Vandal Hearts 1/2 Fix */
	Config.VSyncWA=0; /* 1=InuYasha Sengoku Battle Fix */
	Config.SpuIrq=0; /* 1=SPU IRQ always on, fixes some games */

	Config.AnalogArrow = 0; /* 0=disable 1=use L-stick as D-pad arrow key */
	Config.AnalogMode = 3; /* 0-Digital 1-DualAnalog 2-DualShock (digital mode) 3-DualShock (analog enabled) */
	Config.RumbleGain = 100; /* [0,100]-Rumble effect strength */
	/* Gamepad combo used to open menu
	 * 0: L3 + R3
	 * 1: SELECT + START
	 * 2: None */
	Config.MenuToggleCombo = 0;

	Config.SyncAudio=0;	/* 1=emu waits if audio output buffer is full
	                       (happens seldom with new auto frame limit) */

	// Number of times per frame to update SPU. Rearmed default is once per
	//  frame, but we are more flexible (for slower devices).
	//  Valid values: SPU_UPDATE_FREQ_1 .. SPU_UPDATE_FREQ_32
	Config.SpuUpdateFreq = SPU_UPDATE_FREQ_DEFAULT;

	//senquack - Added option to allow queuing CDREAD_INT interrupts sooner
	//           than they'd normally be issued when SPU's XA buffer is not
	//           full. This fixes droupouts in music/speech on slow devices.
	Config.ForcedXAUpdates = FORCED_XA_UPDATES_DEFAULT;

	Config.ShowFps=0;    // 0=don't show FPS
	Config.FrameLimit = true;
	Config.FrameSkip = FRAMESKIP_OFF;
	Config.VideoScaling = 0;
	Config.VideoHwKeepAspect = true;
	Config.VideoHwFilter = (u8)IPU_FILTER_BICUBIC;

	//zear - Added option to store the last visited directory.
	strncpy(Config.LastDir, home, MAXPATHLEN); /* Defaults to home directory. */
	Config.LastDir[MAXPATHLEN-1] = '\0';

	// senquack - added spu_pcsxrearmed plugin:
#ifdef SPU_PCSXREARMED
	//ORIGINAL PCSX ReARMed SPU defaults (put here for reference):
	//	spu_config.iUseReverb = 1;
	//	spu_config.iUseInterpolation = 1;
	//	spu_config.iXAPitch = 0;
	//	spu_config.iVolume = 768;
	//	spu_config.iTempo = 0;
	//	spu_config.iUseThread = 1; // no effect if only 1 core is detected
	//	// LOW-END DEVICE:
	//	#ifdef HAVE_PRE_ARMV7 /* XXX GPH hack */
	//		spu_config.iUseReverb = 0;
	//		spu_config.iUseInterpolation = 0;
	//		spu_config.iTempo = 1;
	//	#endif

	// PCSX4ALL defaults:
	// NOTE: iUseThread *will* have an effect even on a single-core device, but
	//		 results have yet to be tested. TODO: test if using iUseThread can
	//		 improve sound dropouts in any cases.
	spu_config.iHaveConfiguration = 1;    // *MUST* be set to 1 before calling SPU_Init()
	spu_config.iUseReverb = 0;
	spu_config.iUseInterpolation = 0;
	spu_config.iXAPitch = 0;
	spu_config.iVolume = 1024;            // 1024 is max volume
	spu_config.iUseThread = 0;            // no effect if only 1 core is detected
	spu_config.iUseFixedUpdates = 1;      // This is always set to 1 in libretro's pcsxReARMed
	spu_config.iTempo = 1;                // see note below
#endif

	//senquack - NOTE REGARDING iTempo config var above
	// From thread https://pyra-handheld.com/boards/threads/pcsx-rearmed-r22-now-using-the-dsp.75388/
	// Notaz says that setting iTempo=1 restores pcsxreARMed SPU's old behavior, which allows slow emulation
	// to not introduce audio dropouts (at least I *think* he's referring to iTempo config setting)
	// "Probably the main change is SPU emulation, there were issues in some games where effects were wrong,
	//  mostly Final Fantasy series, it should be better now. There were also sound sync issues where game would
	//  occasionally lock up (like Valkyrie Profile), it should be stable now.
	//  Changed sync has a side effect however - if the emulator is not fast enough (may happen with double
	//  resolution mode or while underclocking), sound will stutter more instead of slowing down the music itself.
	//  There is a new option in SPU plugin config to restore old inaccurate behavior if anyone wants it." -Notaz

	// gpu_dfxvideo
#ifdef GPU_DFXVIDEO
	extern int UseFrameLimit; UseFrameLimit=0; // limit fps 1=on, 0=off
	extern int UseFrameSkip; UseFrameSkip=0; // frame skip 1=on, 0=off
	extern int iFrameLimit; iFrameLimit=0; // fps limit 2=auto 1=fFrameRate, 0=off
	//senquack - TODO: is this really wise to have set to 200 as default:
	extern float fFrameRate; fFrameRate=200.0f; // fps
	extern int iUseDither; iUseDither=0; // 0=off, 1=game dependant, 2=always
	extern int iUseFixes; iUseFixes=0; // use game fixes
	extern uint32_t dwCfgFixes; dwCfgFixes=0; // game fixes
	/*
	 1=odd/even hack (Chrono Cross)
	 2=expand screen width (Capcom fighting games)
	 4=ignore brightness color (black screens in Lunar)
	 8=disable coordinate check (compatibility mode)
	 16=disable cpu saving (for precise framerate)
	 32=PC fps calculation (better fps limit in some games)
	 64=lazy screen update (Pandemonium 2)
	 128=old frame skipping (skip every second frame)
	 256=repeated flat tex triangles (Dark Forces)
	 512=draw quads with triangles (better g-colors, worse textures)
	*/
#endif //GPU_DFXVIDEO

	// gpu_drhell
#ifdef GPU_DRHELL
	extern unsigned int autoFrameSkip; autoFrameSkip=1; /* auto frameskip */
	extern signed int framesToSkip; framesToSkip=0; /* frames to skip */
#endif //GPU_DRHELL

	// gpu_unai
#ifdef GPU_UNAI
	gpu_unai_config_ext.ilace_force = 0;
	gpu_unai_config_ext.pixel_skip = 0;
	gpu_unai_config_ext.lighting = 1;
	gpu_unai_config_ext.fast_lighting = 1;
	gpu_unai_config_ext.blending = 1;
	gpu_unai_config_ext.dithering = 0;
	gpu_unai_config_ext.ntsc_fix = 1;
#endif

	// Load default config from file.
	config_load(NULL);
	// Check if LastDir exists.
	probe_lastdir();

	// command line options
	bool param_parse_error = 0;
	for (int i = 1; i < argc; i++) {
		// PCSX
		// XA audio disabled
		if (strcmp(argv[i],"-noxa") == 0)
			Config.Xa = 1;

		// Black & White MDEC
		if (strcmp(argv[i],"-bwmdec") == 0)
			Config.Mdec = 1;

		// Force PAL system
		if (strcmp(argv[i],"-pal") == 0) {
			Config.PsxAuto = 0;
			Config.PsxType = 1;
		}

		// Force NTSC system
		if (strcmp(argv[i],"-ntsc") == 0) {
			Config.PsxAuto = 0;
			Config.PsxType = 0;
		}

		// CD audio disabled
		if (strcmp(argv[i],"-nocdda") == 0)
			Config.Cdda = 1;

		// BIOS enabled
		if (strcmp(argv[i],"-bios") == 0)
			Config.HLE = 0;

		// Interpreter enabled
		if (strcmp(argv[i],"-interpreter") == 0)
			Config.Cpu = 1;

		// Show BIOS logo sequence at BIOS startup (doesn't apply to HLE)
		if (strcmp(argv[i],"-slowboot") == 0)
			Config.SlowBoot = 1;

		// Parasite Eve 2, Vandal Hearts 1/2 Fix
		if (strcmp(argv[i],"-rcntfix") == 0)
			Config.RCntFix = 1;

		// InuYasha Sengoku Battle Fix
		if (strcmp(argv[i],"-vsyncwa") == 0)
			Config.VSyncWA = 1;

		// SPU IRQ always enabled (fixes audio in some games)
		if (strcmp(argv[i],"-spuirq") == 0)
			Config.SpuIrq = 1;

		// Set ISO file
		if (strcmp(argv[i],"-iso") == 0)
			SetIsoFile(argv[i + 1]);

		// Set executable file
		if (strcmp(argv[i],"-file") == 0)
			strcpy(filename, argv[i + 1]);

		// Audio synchronization option: if audio buffer full, main thread
		//  blocks. Otherwise, just drop the samples.
		if (strcmp(argv[i],"-syncaudio") == 0)
			Config.SyncAudio = 0;

		// Number of times per frame to update SPU. PCSX Rearmed default is once
		//  per frame, but we are more flexible. Valid value is 0..5, where
		//  0 is once per frame, 5 is 32 times per frame (2^5)
		if (strcmp(argv[i],"-spuupdatefreq") == 0) {
			int val = -1;
			if (++i < argc) {
				val = atoi(argv[i]);
				if (val >= SPU_UPDATE_FREQ_MIN && val <= SPU_UPDATE_FREQ_MAX) {
					Config.SpuUpdateFreq = val;
				} else val = -1;
			} else {
				printf("ERROR: missing value for -spuupdatefreq\n");
			}

			if (val == -1) {
				printf("ERROR: -spuupdatefreq value must be between %d..%d\n"
					   "(%d is once per frame)\n",
					   SPU_UPDATE_FREQ_MIN, SPU_UPDATE_FREQ_MAX, SPU_UPDATE_FREQ_1);
				param_parse_error = true;
				break;
			}
		}

		//senquack - Added option to allow queuing CDREAD_INT interrupts sooner
		//           than they'd normally be issued when SPU's XA buffer is not
		//           full. This fixes droupouts in music/speech on slow devices.
		if (strcmp(argv[i],"-forcedxaupdates") == 0) {
			int val = -1;
			if (++i < argc) {
				val = atoi(argv[i]);
				if (val >= FORCED_XA_UPDATES_MIN && val <= FORCED_XA_UPDATES_MAX) {
					Config.ForcedXAUpdates = val;
				} else val = -1;
			} else {
				printf("ERROR: missing value for -forcedxaupdates\n");
			}

			if (val == -1) {
				printf("ERROR: -forcedxaupdates value must be between %d..%d\n",
					   FORCED_XA_UPDATES_MIN, FORCED_XA_UPDATES_MAX);
				param_parse_error = true;
				break;
			}
		}

		// Performance monitoring options
		if (strcmp(argv[i],"-perfmon") == 0) {
			// Enable detailed stats and console output
			Config.PerfmonConsoleOutput = true;
			Config.PerfmonDetailedStats = true;
		}

		// GPU
		// show FPS
		if (strcmp(argv[i],"-showfps") == 0) {
			Config.ShowFps = true;
		}

		// frame limit
		if (strcmp(argv[i],"-noframelimit") == 0) {
			Config.FrameLimit = 0;
		}

		// frame skip
		if (strcmp(argv[i],"-frameskip") == 0) {
			int val = -1000;
			if (++i < argc) {
				val = atoi(argv[i]);
				if (val >= -1 && val <= 3) {
					Config.FrameSkip = val;
				}
			} else {
				printf("ERROR: missing value for -frameskip\n");
			}

			if (val == -1000) {
				printf("ERROR: -frameskip value must be between -1..3 (-1 is AUTO)\n");
				param_parse_error = true;
				break;
			}
		}

#ifdef GPU_UNAI
		// Render only every other line (looks ugly but faster)
		if (strcmp(argv[i],"-interlace") == 0) {
			gpu_unai_config_ext.ilace_force = 1;
		}

		// Allow 24bpp->15bpp dithering (only polys, only if PS1 game uses it)
		if (strcmp(argv[i],"-dither") == 0) {
			gpu_unai_config_ext.dithering = 1;
		}

		if (strcmp(argv[i],"-ntsc_fix") == 0) {
			gpu_unai_config_ext.ntsc_fix = 1;
		}

		if (strcmp(argv[i],"-nolight") == 0) {
			gpu_unai_config_ext.lighting = 0;
		}

		if (strcmp(argv[i],"-noblend") == 0) {
			gpu_unai_config_ext.blending = 0;
		}

		// Apply lighting to all primitives. Default is to only light primitives
		//  with light values below a certain threshold (for speed).
		if (strcmp(argv[i],"-nofastlight") == 0) {
			gpu_unai_config_ext.fast_lighting = 0;
		}

		// Render all pixels on a horizontal line, even when in hi-res 512,640
		//  PSX vid modes and those pixels would never appear on 320x240 screen.
		//  (when using pixel-dropping downscaler).
		//  Can cause visual artifacts, default is on for now (for speed)
		if (strcmp(argv[i],"-nopixelskip") == 0) {
		 	gpu_unai_config_ext.pixel_skip = 0;
		}

		// Settings specific to older, non-gpulib standalone gpu_unai:
#ifndef USE_GPULIB
		// Progressive interlace option - See gpu_unai/gpu.h
		// Old option left in from when PCSX4ALL ran on very slow devices.
		if (strcmp(argv[i],"-progressive") == 0) {
			gpu_unai_config_ext.prog_ilace = 1;
		}
#endif //!USE_GPULIB
#endif //GPU_UNAI


	// SPU
#ifndef SPU_NULL

	// ----- BEGIN SPU_PCSXREARMED SECTION -----
#ifdef SPU_PCSXREARMED
		// No sound
		if (strcmp(argv[i],"-silent") == 0) {
			spu_config.iDisabled = 1;
		}
		// Reverb
		if (strcmp(argv[i],"-reverb") == 0) {
			spu_config.iUseReverb = 1;
		}
		// XA Pitch change support
		if (strcmp(argv[i],"-xapitch") == 0) {
			spu_config.iXAPitch = 1;
		}

		// Enable SPU thread
		// NOTE: By default, PCSX ReARMed would not launch
		//  a thread if only one core was detected, but I have
		//  changed it to allow it under any case.
		// TODO: test if any benefit is ever achieved
		if (strcmp(argv[i],"-threaded_spu") == 0) {
			spu_config.iUseThread = 1;
		}

		// Don't output fixed number of samples per frame
		// (unknown if this helps or hurts performance
		//  or compatibility.) The default in all builds
		//  of PCSX_ReARMed is "true", so that is also the
		//  default here.
		if (strcmp(argv[i],"-nofixedupdates") == 0) {
			spu_config.iUseFixedUpdates = 0;
		}

		// Set interpolation none/simple/gaussian/cubic, default is none
		if (strcmp(argv[i],"-interpolation") == 0) {
			int val = -1;
			if (++i < argc) {
				if (strcmp(argv[i],"none") == 0) val=0;
				if (strcmp(argv[i],"simple") == 0) val=1;
				if (strcmp(argv[i],"gaussian") == 0) val=2;
				if (strcmp(argv[i],"cubic") == 0) val=3;
			} else
				printf("ERROR: missing value for -interpolation\n");


			if (val == -1) {
				printf("ERROR: -interpolation value must be one of: none,simple,gaussian,cubic\n");
				param_parse_error = true; break;
			}

			spu_config.iUseInterpolation = val;
		}

		// Set volume level of SPU, 0-1024
		//  If value is 0, sound will be disabled.
		if (strcmp(argv[i],"-volume") == 0) {
			int val = -1;
			if (++i < argc)
				val = atoi(argv[i]);
			else
				printf("ERROR: missing value for -volume\n");

			if (val < 0 || val > 1024) {
				printf("ERROR: -volume value must be between 0-1024. Value of 0 will mute sound\n"
						"        but SPU plugin will still run, ensuring best compatibility.\n"
						"        Use -silent flag to disable SPU plugin entirely.\n");
				param_parse_error = true; break;
			}

			spu_config.iVolume = val;
		}

		// SPU will issue updates at a rate that ensures better
		//  compatibility, but if the emulator runs too slowly,
		//  audio stutter will be increased. "False" is the
		//  default setting on Pandora/Pyra/Android builds of
		//  PCSX_ReARMed, but Wiz/Caanoo builds used the faster
		//  inaccurate setting, "true", so I've made our default
		//  "true" as well, since we target low-end devices.
		if (strcmp(argv[i],"-notempo") == 0) {
			spu_config.iTempo = 0;
		}

		//NOTE REGARDING ABOVE SETTING "spu_config.iTempo":
		// From thread https://pyra-handheld.com/boards/threads/pcsx-rearmed-r22-now-using-the-dsp.75388/
		// Notaz says that setting iTempo=1 restores pcsxreARMed SPU's old behavior, which allows slow emulation
		// to not introduce audio dropouts (at least I *think* he's referring to iTempo config setting)
		// "Probably the main change is SPU emulation, there were issues in some games where effects were wrong,
		//  mostly Final Fantasy series, it should be better now. There were also sound sync issues where game would
		//  occasionally lock up (like Valkyrie Profile), it should be stable now.
		//  Changed sync has a side effect however - if the emulator is not fast enough (may happen with double
		//  resolution mode or while underclocking), sound will stutter more instead of slowing down the music itself.
		//  There is a new option in SPU plugin config to restore old inaccurate behavior if anyone wants it." -Notaz

#endif //SPU_PCSXREARMED
	// ----- END SPU_PCSXREARMED SECTION -----

#endif //!SPU_NULL
	}

	if (param_parse_error) {
		printf("Failed to parse command-line parameters, exiting.\n");
		exit(1);
	}

	/* If we have a valid filename at this point,
	 * then config overrides are enabled
	 * > Set CdromName and check whether an
	 *   override exists */
	if (!string_is_empty(cdrfilename)) {
		set_cdrom_name(cdrfilename);
	} else if (!string_is_empty(filename)) {
		set_cdrom_name(filename);
	}

	if (!string_is_empty(CdromName)) {
		char config_file[MAXPATHLEN];
		config_override_enabled = 1;
		config_get_override_filename(CdromName, config_file);
		if (path_file_exists(config_file)) {
			config_override_active = 1;
			config_load(CdromName);
			probe_lastdir();
		}
	}

	update_memcards(0);
	strcpy(BiosFile, Config.Bios);

#ifdef GCW_ZERO
	/* Apply hardware scaling 'keep aspect' setting */
	set_ipu_keep_aspect_ratio(Config.VideoHwKeepAspect);
	/* Apply hardware scaling 'filter' setting */
	set_ipu_filter_type((enum ipu_filter_type)Config.VideoHwFilter);
#endif

	//NOTE: spu_pcsxrearmed will handle audio initialization

	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_NOPARACHUTE);
	SDL_JoystickEventState(SDL_ENABLE);

	atexit(pcsx4all_exit);

	SDL_WM_SetCaption("pcsx4all - SDL Version", "pcsx4all");

	if (Config.VideoScaling == 1) {
#ifdef SDL_TRIPLEBUF
	int flags = SDL_TRIPLEBUF;
#else
	int flags = SDL_DOUBLEBUF;
#endif
    flags |= SDL_HWSURFACE
#if defined(GCW_ZERO) && defined(USE_BGR15)
        | SDL_SWIZZLEBGR
#endif
        ;
		SCREEN_WIDTH = 320;
		SCREEN_HEIGHT = 240;

		if (screen && SDL_MUSTLOCK(screen))
			SDL_UnlockSurface(screen);

		screen = SDL_SetVideoMode(SCREEN_WIDTH, SCREEN_HEIGHT,
#if !defined(GCW_ZERO) || !defined(USE_BGR15)
			16,
#else
			15,
#endif
		flags);
		if (!screen) {
			puts("NO Set VideoMode 320x240x16");
			exit(0);
		}

		if (SDL_MUSTLOCK(screen))
			SDL_LockSurface(screen);

		SCREEN = (Uint16 *) screen->pixels;
	} else {
		update_window_size(320, 240, false);
	}

	if (argc < 2 || string_is_empty(cdrfilename)) {
		// Enter frontend main-menu:
		emu_running = false;
		if (!SelectGame()) {
			printf("ERROR: missing filename for -iso\n");
			exit(1);
		}
	}

	if (psxInit() == -1) {
		printf("PSX emulator couldn't be initialized.\n");
		exit(1);
	}

	if (LoadPlugins() == -1) {
		printf("Failed loading plugins.\n");
		exit(1);
	}

	pcsx4all_initted = true;
	emu_running = true;

	// Initialize plugin_lib, gpulib
	pl_init();

	if (!string_is_empty(cdrfilename)) {
		if (CheckCdrom() == -1) {
			psxReset();
			printf("Failed checking ISO image.\n");
			SetIsoFile(NULL);
		} else {
			check_spec_bios();
			psxReset();
			printf("Running ISO image: %s.\n", cdrfilename);
			if (LoadCdrom() == -1) {
				printf("Failed loading ISO image.\n");
				SetIsoFile(NULL);
			} else {
				// load cheats
				cheat_load();
			}
		}
	} else {
		psxReset();
	}

	/* If we are using per-disk memory cards
	or disable one or both of them, load them now */
	if ((Config.McdSlot1 == 17) || (Config.McdSlot2 == 17)) {
		update_memcards(0);
		LoadMcd(MCD1, (char*)GetMemcardPath(1)); //Memcard 1
		LoadMcd(MCD2, (char*)GetMemcardPath(2)); //Memcard 2
	}

	Rumble_Init();
	joy_init();

	if (!string_is_empty(filename)) {
		if (Load(filename) == -1) {
			printf("Failed loading executable.\n");
			filename[0]='\0';
		}

		printf("Running executable: %s.\n",filename);
	}

	if (string_is_empty(cdrfilename) && string_is_empty(filename) && (Config.HLE == 0)) {
		printf("Running BIOS.\n");
	}

	if (!string_is_empty(cdrfilename) || !string_is_empty(filename) || (Config.HLE == 0)) {
		psxCpu->Execute();
	}

	return 0;
}

unsigned get_ticks(void)
{
#ifdef TIME_IN_MSEC
	return SDL_GetTicks();
#else
	return ((((unsigned long long)clock())*1000000ULL)/((unsigned long long)CLOCKS_PER_SEC));
#endif
}

void wait_ticks(unsigned s)
{
#ifdef TIME_IN_MSEC
	SDL_Delay(s);
#else
	SDL_Delay(s/1000);
#endif
}

void port_printf(int x, int y, const char *text)
{
	static const unsigned char fontdata8x8[] =
	{
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x3C,0x42,0x99,0xBD,0xBD,0x99,0x42,0x3C,0x3C,0x42,0x81,0x81,0x81,0x81,0x42,0x3C,
		0xFE,0x82,0x8A,0xD2,0xA2,0x82,0xFE,0x00,0xFE,0x82,0x82,0x82,0x82,0x82,0xFE,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x38,0x64,0x74,0x7C,0x38,0x00,0x00,
		0x80,0xC0,0xF0,0xFC,0xF0,0xC0,0x80,0x00,0x01,0x03,0x0F,0x3F,0x0F,0x03,0x01,0x00,
		0x18,0x3C,0x7E,0x18,0x7E,0x3C,0x18,0x00,0xEE,0xEE,0xEE,0xCC,0x00,0xCC,0xCC,0x00,
		0x00,0x00,0x30,0x68,0x78,0x30,0x00,0x00,0x00,0x38,0x64,0x74,0x7C,0x38,0x00,0x00,
		0x3C,0x66,0x7A,0x7A,0x7E,0x7E,0x3C,0x00,0x0E,0x3E,0x3A,0x22,0x26,0x6E,0xE4,0x40,
		0x18,0x3C,0x7E,0x3C,0x3C,0x3C,0x3C,0x00,0x3C,0x3C,0x3C,0x3C,0x7E,0x3C,0x18,0x00,
		0x08,0x7C,0x7E,0x7E,0x7C,0x08,0x00,0x00,0x10,0x3E,0x7E,0x7E,0x3E,0x10,0x00,0x00,
		0x58,0x2A,0xDC,0xC8,0xDC,0x2A,0x58,0x00,0x24,0x66,0xFF,0xFF,0x66,0x24,0x00,0x00,
		0x00,0x10,0x10,0x38,0x38,0x7C,0xFE,0x00,0xFE,0x7C,0x38,0x38,0x10,0x10,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1C,0x1C,0x1C,0x18,0x00,0x18,0x18,0x00,
		0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x28,0x7C,0x28,0x7C,0x28,0x00,0x00,
		0x10,0x38,0x60,0x38,0x0C,0x78,0x10,0x00,0x40,0xA4,0x48,0x10,0x24,0x4A,0x04,0x00,
		0x18,0x34,0x18,0x3A,0x6C,0x66,0x3A,0x00,0x18,0x18,0x20,0x00,0x00,0x00,0x00,0x00,
		0x30,0x60,0x60,0x60,0x60,0x60,0x30,0x00,0x0C,0x06,0x06,0x06,0x06,0x06,0x0C,0x00,
		0x10,0x54,0x38,0x7C,0x38,0x54,0x10,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,
		0x00,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x3E,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x04,0x08,0x10,0x20,0x40,0x00,0x00,
		0x38,0x4C,0xC6,0xC6,0xC6,0x64,0x38,0x00,0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00,
		0x7C,0xC6,0x0E,0x3C,0x78,0xE0,0xFE,0x00,0x7E,0x0C,0x18,0x3C,0x06,0xC6,0x7C,0x00,
		0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x00,0xFC,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00,
		0x3C,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00,0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00,
		0x78,0xC4,0xE4,0x78,0x86,0x86,0x7C,0x00,0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00,
		0x00,0x00,0x18,0x00,0x00,0x18,0x00,0x00,0x00,0x00,0x18,0x00,0x00,0x18,0x18,0x30,
		0x1C,0x38,0x70,0xE0,0x70,0x38,0x1C,0x00,0x00,0x7C,0x00,0x00,0x7C,0x00,0x00,0x00,
		0x70,0x38,0x1C,0x0E,0x1C,0x38,0x70,0x00,0x7C,0xC6,0xC6,0x1C,0x18,0x00,0x18,0x00,
		0x3C,0x42,0x99,0xA1,0xA5,0x99,0x42,0x3C,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00,
		0xFC,0xC6,0xC6,0xFC,0xC6,0xC6,0xFC,0x00,0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00,
		0xF8,0xCC,0xC6,0xC6,0xC6,0xCC,0xF8,0x00,0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xFE,0x00,
		0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xC0,0x00,0x3E,0x60,0xC0,0xCE,0xC6,0x66,0x3E,0x00,
		0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,
		0x06,0x06,0x06,0x06,0xC6,0xC6,0x7C,0x00,0xC6,0xCC,0xD8,0xF0,0xF8,0xDC,0xCE,0x00,
		0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00,
		0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,
		0xFC,0xC6,0xC6,0xC6,0xFC,0xC0,0xC0,0x00,0x7C,0xC6,0xC6,0xC6,0xDE,0xCC,0x7A,0x00,
		0xFC,0xC6,0xC6,0xCE,0xF8,0xDC,0xCE,0x00,0x78,0xCC,0xC0,0x7C,0x06,0xC6,0x7C,0x00,
		0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,
		0xC6,0xC6,0xC6,0xEE,0x7C,0x38,0x10,0x00,0xC6,0xC6,0xD6,0xFE,0xFE,0xEE,0xC6,0x00,
		0xC6,0xEE,0x3C,0x38,0x7C,0xEE,0xC6,0x00,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00,
		0xFE,0x0E,0x1C,0x38,0x70,0xE0,0xFE,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,
		0x60,0x60,0x30,0x18,0x0C,0x06,0x06,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,
		0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,
		0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x3C,0x06,0x3E,0x66,0x66,0x3C,0x00,
		0x60,0x7C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x3C,0x66,0x60,0x60,0x66,0x3C,0x00,
		0x06,0x3E,0x66,0x66,0x66,0x66,0x3E,0x00,0x00,0x3C,0x66,0x66,0x7E,0x60,0x3C,0x00,
		0x1C,0x30,0x78,0x30,0x30,0x30,0x30,0x00,0x00,0x3E,0x66,0x66,0x66,0x3E,0x06,0x3C,
		0x60,0x7C,0x76,0x66,0x66,0x66,0x66,0x00,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x00,
		0x0C,0x00,0x1C,0x0C,0x0C,0x0C,0x0C,0x38,0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00,
		0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0xEC,0xFE,0xFE,0xFE,0xD6,0xC6,0x00,
		0x00,0x7C,0x76,0x66,0x66,0x66,0x66,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x3C,0x00,
		0x00,0x7C,0x66,0x66,0x66,0x7C,0x60,0x60,0x00,0x3E,0x66,0x66,0x66,0x3E,0x06,0x06,
		0x00,0x7E,0x70,0x60,0x60,0x60,0x60,0x00,0x00,0x3C,0x60,0x3C,0x06,0x66,0x3C,0x00,
		0x30,0x78,0x30,0x30,0x30,0x30,0x1C,0x00,0x00,0x66,0x66,0x66,0x66,0x6E,0x3E,0x00,
		0x00,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x7C,0x6C,0x00,
		0x00,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x06,0x3C,
		0x00,0x7E,0x0C,0x18,0x30,0x60,0x7E,0x00,0x0E,0x18,0x0C,0x38,0x0C,0x18,0x0E,0x00,
		0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00,0x70,0x18,0x30,0x1C,0x30,0x18,0x70,0x00,
		0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x10,0x28,0x10,0x54,0xAA,0x44,0x00,0x00,
	};
	unsigned short *screen = (SCREEN + x + y * SCREEN_WIDTH);
	int len = strlen(text);
	for (int i = 0; i < len; i++) {
		int pos = 0;
		for (int l = 0; l < 8; l++) {
			unsigned char data = fontdata8x8[((text[i])*8)+l];
			if (data&0x80u) screen[pos+0] = ~screen[pos+0];
			if (data&0x40u) screen[pos+1] = ~screen[pos+1];
			if (data&0x20u) screen[pos+2] = ~screen[pos+2];
			if (data&0x10u) screen[pos+3] = ~screen[pos+3];
			if (data&0x08u) screen[pos+4] = ~screen[pos+4];
			if (data&0x04u) screen[pos+5] = ~screen[pos+5];
			if (data&0x02u) screen[pos+6] = ~screen[pos+6];
			if (data&0x01u) screen[pos+7] = ~screen[pos+7];
			pos += SCREEN_WIDTH;
		}
		screen += 8;
	}
}
