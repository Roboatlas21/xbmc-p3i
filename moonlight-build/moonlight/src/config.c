/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "platform.h"
#include "config.h"
#include "util.h"
#include "cpu.h"
#include "config_numeric.h"

#include "input/evdev.h"
#include "audio/audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <pwd.h>
#include <sys/types.h>

#define MOONLIGHT_PATH "/moonlight"
#define USER_PATHS "."
#define DEFAULT_CONFIG_DIR "/.config"
#define DEFAULT_CACHE_DIR "/.cache"

#define write_config_string(fd, key, value) fprintf(fd, "%s = %s\n", key, value)
#define write_config_int(fd, key, value) fprintf(fd, "%s = %d\n", key, value)
#define write_config_bool(fd, key, value) fprintf(fd, "%s = %s\n", key, value ? "true":"false")

bool inputAdded = false;

static struct option long_options[] = {
  {"720", no_argument, NULL, 'a'},
  {"1080", no_argument, NULL, 'b'},
  {"4k", no_argument, NULL, '0'},
  {"width", required_argument, NULL, 'c'},
  {"height", required_argument, NULL, 'd'},
  {"bitrate", required_argument, NULL, 'g'},
  {"packetsize", required_argument, NULL, 'h'},
  {"app", required_argument, NULL, 'i'},
  {"input", required_argument, NULL, 'j'},
  {"mapping", required_argument, NULL, 'k'},
  {"nosops", no_argument, NULL, 'l'},
  {"audio", required_argument, NULL, 'm'},
  {"localaudio", no_argument, NULL, 'n'},
  {"config", required_argument, NULL, 'o'},
  {"platform", required_argument, NULL, 'p'},
  {"save", required_argument, NULL, 'q'},
  {"keydir", required_argument, NULL, 'r'},
  {"remote", required_argument, NULL, 's'},
  {"windowed", no_argument, NULL, 't'},
  {"surround", required_argument, NULL, 'u'},
  {"fps", required_argument, NULL, 'v'},
  {"codec", required_argument, NULL, 'x'},
  {"nounsupported", no_argument, NULL, 'y'},
  {"quitappafter", no_argument, NULL, '1'},
  {"viewonly", no_argument, NULL, '2'},
  {"rotate", required_argument, NULL, '3'},
  {"verbose", no_argument, NULL, 'z'},
  {"debug", no_argument, NULL, 'Z'},
  {"nomouseemulation", no_argument, NULL, '4'},
  {"pin", required_argument, NULL, '5'},
  {"port", required_argument, NULL, '6'},
  {"hdr", no_argument, NULL, '7'},
  {"cec", required_argument, NULL, 'C'},
  {"osd", no_argument, NULL, '8'},
  {"osdargs", required_argument, NULL, '9'},
  {0, 0, 0, 0},
};

char* get_path(char* name, char* extra_data_dirs) {
  const char *xdg_config_dir = getenv("XDG_CONFIG_DIR");
  const char *home_dir = getenv("HOME");

  if (access(name, R_OK) != -1) {
      return name;
  }

  if (!home_dir) {
    struct passwd *pw = getpwuid(getuid());
    if (!pw || !pw->pw_dir || !*pw->pw_dir) {
      fprintf(stderr, "Cannot determine home directory for Moonlight configuration\n");
      return NULL;
    }
    home_dir = pw->pw_dir;
  }

  if (!extra_data_dirs)
    extra_data_dirs = "/usr/share:/usr/local/share";
  if (!xdg_config_dir)
    xdg_config_dir = home_dir;

  char *data_dirs = malloc(strlen(USER_PATHS) + 1 + strlen(xdg_config_dir) + 1 + strlen(home_dir) + 1 + strlen(DEFAULT_CONFIG_DIR) + 1 + strlen(extra_data_dirs) + 2);
  if (!data_dirs) {
    fprintf(stderr, "Not enough memory for Moonlight configuration paths\n");
    return NULL;
  }
  sprintf(data_dirs, USER_PATHS ":%s:%s/" DEFAULT_CONFIG_DIR ":%s/", xdg_config_dir, home_dir, extra_data_dirs);

  char *path = malloc(strlen(data_dirs)+strlen(MOONLIGHT_PATH)+strlen(name)+2);
  if (path == NULL) {
    fprintf(stderr, "Not enough memory\n");
    exit(-1);
  }

  char *data_dir = data_dirs;
  for (;;) {
    char *end = strchr(data_dir, ':');
    size_t length = end ? (size_t)(end - data_dir) : strlen(data_dir);
    if (length) {
      memcpy(path, data_dir, length);
      path[length] = '\0';
      if (path[0] == '/')
        sprintf(path+length, MOONLIGHT_PATH "/%s", name);
      else
        sprintf(path+length, "/%s", name);

      if(access(path, R_OK) != -1) {
        free(data_dirs);
        return path;
      }
    }

    if (!end)
      break; /* Never form an out-of-bounds pointer from NULL. */
    data_dir = end + 1;
  }

  free(data_dirs);
  free(path);
  return NULL;
}

static int parse_stream_number(const char *option, const char *value,
                               int minimum, int maximum) {
  int parsed = 0;
  if (!moonlight_parse_bounded_int(value, minimum, maximum, &parsed)) {
    fprintf(stderr, "Invalid %s: expected integer between %d and %d\n",
            option, minimum, maximum);
    exit(EXIT_FAILURE);
  }
  return parsed;
}

static void parse_argument(int c, char* value, PCONFIGURATION config) {
  switch (c) {
  case 'a':
    config->stream.width = 1280;
    config->stream.height = 720;
    break;
  case 'b':
    config->stream.width = 1920;
    config->stream.height = 1080;
    break;
  case '0':
    config->stream.width = 3840;
    config->stream.height = 2160;
    break;
  case 'c':
    config->stream.width = parse_stream_number("width", value, 64, 7680);
    break;
  case 'd':
    config->stream.height = parse_stream_number("height", value, 64, 4320);
    break;
  case 'g':
    config->stream.bitrate = parse_stream_number("bitrate", value, -1, 500000);
    break;
  case 'h':
    config->stream.packetSize = parse_stream_number("packetsize", value, 256, 1500);
    break;
  case 'i':
    config->app = value;
    break;
  case 'j':
    if (config->inputsCount >= MAX_INPUTS) {
      perror("Too many inputs specified");
      exit(-1);
    }
    config->inputs[config->inputsCount] = value;
    config->inputsCount++;
    inputAdded = true;
    break;
  case 'k':
    config->mapping = get_path(value, getenv("XDG_DATA_DIRS"));
    if (config->mapping == NULL) {
      fprintf(stderr, "Unable to open custom mapping file: %s\n", value);
      exit(-1);
    }
    break;
  case 'l':
    config->sops = false;
    break;
  case 'm':
    config->audio_device = value;
    break;
  case 'n':
    config->localaudio = true;
    break;
  case 'o':
    if (!config_file_parse(value, config))
      exit(EXIT_FAILURE);

    break;
  case 'p':
    config->platform = value;
    break;
  case 'q':
    config->config_file = value;
    break;
  case 'r':
    /* A key directory is supplied by local configuration/CLI, but it still
     * must fit the fixed credential path buffer without truncation. */
    if (value == NULL || strlen(value) >= sizeof(config->key_dir)) {
      fprintf(stderr, "Key directory path exceeds %zu bytes\n", sizeof(config->key_dir) - 1);
      exit(EXIT_FAILURE);
    }
    memcpy(config->key_dir, value, strlen(value) + 1);
    break;
  case 's':
    if (strcasecmp(value, "auto") == 0)
      config->stream.streamingRemotely = STREAM_CFG_AUTO;
    else if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0)
      config->stream.streamingRemotely = STREAM_CFG_REMOTE;
    else if (strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0)
      config->stream.streamingRemotely = STREAM_CFG_LOCAL;
    break;

  case 't':
    config->fullscreen = false;
    break;
  case 'u':
    if (strcasecmp(value, "5.1") == 0)
      config->stream.audioConfiguration = AUDIO_CONFIGURATION_51_SURROUND;
    else if (strcasecmp(value, "7.1") == 0)
      config->stream.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND;
    break;
  case 'v':
    config->stream.fps = parse_stream_number("fps", value, 1, 240);
    break;
  case 'x':
    if (strcasecmp(value, "auto") == 0)
      config->codec = CODEC_UNSPECIFIED;
    else if (strcasecmp(value, "h264") == 0)
      config->codec = CODEC_H264;
    else if (strcasecmp(value, "h265") == 0 || strcasecmp(value, "hevc") == 0)
      config->codec = CODEC_HEVC;
    else if (strcasecmp(value, "av1") == 0)
      config->codec = CODEC_AV1;
    break;
  case 'y':
    config->unsupported = false;
    break;
  case '1':
    config->quitappafter = true;
    break;
  case '2':
    config->viewonly = true;
    break;
  case '3':
    config->rotate = parse_stream_number("rotation", value, 0, 270);
    if (config->rotate != 0 && config->rotate != 90 &&
        config->rotate != 180 && config->rotate != 270) {
      fprintf(stderr, "Invalid rotation: expected 0, 90, 180, or 270 degrees\n");
      exit(EXIT_FAILURE);
    }
    break;
  case 'z':
    config->debug_level = 1;
    break;
  case 'Z':
    config->debug_level = 2;
    break;
  case '4':
    config->mouse_emulation = false;
    break;
  case '5':
    config->pin = parse_stream_number("pairing PIN", value, 1, 9999);
    break;
  case '6':
    config->port = parse_stream_number("host port", value, 1, 65535);
    break;
  case 'C':
    // Luna writes an explicit boolean; do not treat "cec = false" as enabled.
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0)
      config->cec = true;
    else if (strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0)
      config->cec = false;
    else {
      fprintf(stderr, "Invalid cec setting: expected true/false\n");
      exit(EXIT_FAILURE);
    }
    break;
  case '7':
    config->hdr = true;
    break;
  case '8':
    config->osd = true;
    break;
  case '9':
    config->osdargs = value;
    break;
  case 1:
    if (config->action == NULL)
      config->action = value;
    else if (!config->command_line_address_set) {
      /* A saved config address is only a default. Luna's explicit Quit
       * destination (or any explicitly supplied host) must override it. */
      config->address = value;
      config->command_line_address_set = true;
    } else {
      fprintf(stderr, "Too many positional arguments\n");
      exit(EXIT_FAILURE);
    }
  }
}

bool config_file_parse(char* filename, PCONFIGURATION config) {
  FILE* fd = fopen(filename, "r");
  if (fd == NULL) {
    fprintf(stderr, "Can't open configuration file: %s\n", filename);
    return false;
  }

  char *line = NULL;
  size_t len = 0;
  while (getline(&line, &len, fd) != -1) {
    char *key = NULL, *value = NULL;
    bool retained_value = false;
    if (sscanf(line, "%ms = %m[^\n]", &key, &value) == 2) {
      if (strcmp(key, "address") == 0) {
        config->address = value;
        retained_value = true;
      } else if (strcmp(key, "sops") == 0) {
        config->sops = strcmp("true", value) == 0;
      } else {
        for (int i = 0; long_options[i].name != NULL; i++) {
          if (strcmp(long_options[i].name, key) != 0)
            continue;
          if (long_options[i].has_arg == required_argument) {
            parse_argument(long_options[i].val, value, config);
            /* String options retain the original pointer; numeric/boolean
             * options consume it immediately and may free it here.
             * The mapping path ('k') may be borrowed by get_path(), so it
             * must remain live even if the input file exists in CWD. */
            switch (long_options[i].val) {
            case 'i': case 'j': case 'k': case 'm': case 'p': case 'q': case '9':
              retained_value = true;
              break;
            default: break;
            }
          } else if (strcmp("true", value) == 0) {
            parse_argument(long_options[i].val, NULL, config);
          }
        }
      }
    }
    free(key);
    if (!retained_value) free(value);
  }
  bool read_ok = !ferror(fd);
  free(line);
  if (fclose(fd) != 0) read_ok = false;
  return read_ok;
}

void config_save(char* filename, PCONFIGURATION config) {
  FILE* fd = fopen(filename, "w");
  if (fd == NULL) {
    fprintf(stderr, "Can't open configuration file: %s\n", filename);
    exit(EXIT_FAILURE);
  }

  if (config->stream.width != 1280)
    write_config_int(fd, "width", config->stream.width);
  if (config->stream.height != 720)
    write_config_int(fd, "height", config->stream.height);
  if (config->stream.fps != 60)
    write_config_int(fd, "fps", config->stream.fps);
  if (config->stream.bitrate != -1)
    write_config_int(fd, "bitrate", config->stream.bitrate);
  if (config->stream.packetSize != 1024)
    write_config_int(fd, "packetsize", config->stream.packetSize);
  if (!config->sops)
    write_config_bool(fd, "sops", config->sops);
  if (config->localaudio)
    write_config_bool(fd, "localaudio", config->localaudio);
  if (config->quitappafter)
    write_config_bool(fd, "quitappafter", config->quitappafter);
  if (config->viewonly)
    write_config_bool(fd, "viewonly", config->viewonly);
  if (config->rotate != 0)
    write_config_int(fd, "rotate", config->rotate);
  if (config->cec) write_config_bool(fd, "cec", true);
  if (config->osd) write_config_bool(fd, "osd", true);
  if (config->osdargs) write_config_string(fd, "osdargs", config->osdargs);

  if (strcmp(config->app, "Steam") != 0)
    write_config_string(fd, "app", config->app);

  fclose(fd);
}

void config_parse(int argc, char* argv[], PCONFIGURATION config) {
  LiInitializeStreamConfiguration(&config->stream);

  config->stream.width = 1280;
  config->stream.height = 720;
  config->stream.fps = 60;
  config->stream.bitrate = -1;
  config->stream.packetSize = 1392;
  config->stream.streamingRemotely = STREAM_CFG_AUTO;
  config->stream.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
  config->stream.supportedVideoFormats = SCM_H264;

  // Opt in for video encryption if the CPU has good AES performance
  if (has_fast_aes()) {
    config->stream.encryptionFlags = ENCFLG_ALL;
  }
  else if (has_slow_aes()) {
    // For extremely slow CPUs, opt out of audio encryption
    config->stream.encryptionFlags = ENCFLG_NONE;
    printf("Disabling encryption on low performance CPU\n");
  }
  else {
    config->stream.encryptionFlags = ENCFLG_AUDIO;
  }

  config->debug_level = 0;
  config->platform = "auto";
  config->app = "Steam";
  config->action = NULL;
  config->address = NULL;
  config->command_line_address_set = false;
  config->config_file = NULL;
  config->audio_device = NULL;
  config->sops = true;
  config->localaudio = false;
  config->fullscreen = true;
  config->unsupported = true;
  config->quitappafter = false;
  config->viewonly = false;
  config->mouse_emulation = true;
  config->rotate = 0;
  config->codec = CODEC_UNSPECIFIED;
  config->hdr = false;
  config->cec = false;
  config->osd = false;
  config->osdargs = NULL;
  config->pin = 0;
  config->port = 47989;

  config->inputsCount = 0;
  config->mapping = get_path("gamecontrollerdb.txt", getenv("XDG_DATA_DIRS"));
  config->key_dir[0] = 0;

  /* Luna passes an exact per-session -config file after preflighting that
   * same snapshot. Do not load shared moonlight.conf first: options omitted
   * from the snapshot would otherwise leak through, and repeated input=
   * values could register the same device more than once. */
  const char *session_config = getenv("LUNA_STREAM_CONFIG");
  bool use_session_only = false;
  if (session_config && *session_config) {
    for (int i = 1; i + 1 < argc; ++i) {
      if ((strcmp(argv[i], "-config") == 0 ||
           strcmp(argv[i], "--config") == 0) &&
          strcmp(argv[i + 1], session_config) == 0) {
        use_session_only = true;
        break;
      }
    }
  }
  if (!use_session_only) {
    char *config_file = get_path("moonlight.conf", "/etc");
    if (config_file)
      config_file_parse(config_file, config);
  }

  if (argc == 2 && access(argv[1], F_OK) == 0) {
    config->action = "stream";
    if (!config_file_parse(argv[1], config))
      exit(EXIT_FAILURE);

  } else {
    int option_index = 0;
    int c;
    while ((c = getopt_long_only(argc, argv, "-abc:d:efg:h:i:j:k:lm:no:p:q:r:s:tu:v:w:xy45:6:789:C:", long_options, &option_index)) != -1) {
      parse_argument(c, optarg, config);
    }
  }

  if (config->config_file != NULL)
    config_save(config->config_file, config);

  if (config->key_dir[0] == 0x0) {
    struct passwd *pw = getpwuid(getuid());
    const char *dir;
    if ((dir = getenv("XDG_CACHE_DIR")) != NULL)
      snprintf(config->key_dir, sizeof(config->key_dir), "%s" MOONLIGHT_PATH, dir);
    else if ((dir = getenv("HOME")) != NULL)
      snprintf(config->key_dir, sizeof(config->key_dir), "%s" DEFAULT_CACHE_DIR MOONLIGHT_PATH, dir);
    else if (pw && pw->pw_dir && *pw->pw_dir)
      snprintf(config->key_dir, sizeof(config->key_dir), "%s" DEFAULT_CACHE_DIR MOONLIGHT_PATH, pw->pw_dir);
    else {
      fprintf(stderr, "Cannot determine credential directory without HOME or passwd entry\n");
      exit(EXIT_FAILURE);
    }
  }

  if (!moonlight_stream_numbers_valid(config->stream.width,
                                      config->stream.height,
                                      config->stream.fps,
                                      config->stream.packetSize,
                                      config->stream.bitrate)) {
    fprintf(stderr, "Invalid stream dimensions, FPS, packet size, or bitrate\n");
    exit(EXIT_FAILURE);
  }

  const uint64_t pixels = moonlight_stream_pixel_count(
      config->stream.width, config->stream.height);
  if (config->stream.bitrate == -1) {
    // This table prefers 16:10 resolutions because they are
    // only slightly more pixels than the 16:9 equivalents, so
    // we don't want to bump those 16:10 resolutions up to the
    // next 16:9 slot.

    if (pixels <= 640 * 360) {
      config->stream.bitrate = (int)(1000 * (config->stream.fps / 30.0));
    } else if (pixels <= 854 * 480) {
      config->stream.bitrate = (int)(1500 * (config->stream.fps / 30.0));
    } else if (pixels <= 1366 * 768) {
      // This covers 1280x720 and 1280x800 too
      config->stream.bitrate = (int)(5000 * (config->stream.fps / 30.0));
    } else if (pixels <= 1920 * 1200) {
      config->stream.bitrate = (int)(10000 * (config->stream.fps / 30.0));
    } else if (pixels <= 2560 * 1600) {
      config->stream.bitrate = (int)(20000 * (config->stream.fps / 30.0));
    } else { /* All larger validated resolutions, up to 8K. */
      config->stream.bitrate = (int)(40000 * (config->stream.fps / 30.0));
    }
  }
}
