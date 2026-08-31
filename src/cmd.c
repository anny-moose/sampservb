#include "cmd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "serv.h"
#include "servfetch.h"

extern char** environ;

static void call_quit(const char** argv, void* cfg_) {
    (void)argv;
    struct app_state* cfg = cfg_;

    cfg->quit = true;
}

static void call_echo(const char** argv, void* cfg_) {
    (void)cfg_;
    char buf[64] = {0};

    const char** curs = argv + 1;
    while (*curs != NULL) {
        strcat(buf, *curs);
        curs++;
    }

    notify(buf);
}

static void call_connect(const char** argv, void* cfg_) {
    struct app_state* cfg = cfg_;
    struct tab_state* tab = cfg->tabs + cfg->tabs_selected;
    if (tab->exec_cmd == NULL) {
        notify("exec_cmd Isn't set!");
        return;
    }

    if (*tab->username == '\0') {
        notify("username Isn't set!");
        return;
    }

    if (child_spawned != 0) {
        notify("Game process already exists: %jd", (intmax_t)cfg->game_proc);
        return;
    }

    const char* generic_err =
        "Address wasn't specified properly! Expected :connect <addr> <port>";

    char ip[INET_ADDRSTRLEN];
    int portx;

    if (argv[1] == NULL) {
        if (tab->list == NULL || tab->selected >= tab->list->num_displayed) {
            notify(generic_err);
            return;
        }

        const struct servinfo* serv = tab->list->servs + tab->selected;

        inet_ntop(AF_INET, &serv->ip, ip, INET_ADDRSTRLEN);
        portx = ntohs(serv->port);
        goto launch;
    }

    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_protocol = SOCK_DGRAM,
    };

    struct addrinfo* resp;

    int ret = getaddrinfo(argv[1], NULL, &hints, &resp);
    if (ret != 0) {
        notify("Failed to resolve address: %s", gai_strerror(ret));
        return;
    }
    if (resp->ai_addr->sa_family != AF_INET) {
        notify("Failed to resolve address");
        freeaddrinfo(resp);
        return;
    }
    struct sockaddr_in addr = *(struct sockaddr_in*)(resp->ai_addr);
    freeaddrinfo(resp);
    inet_ntop(AF_INET, &addr.sin_addr, ip, 15);

    if (argv[2] == NULL) {
        portx = 7777;
    } else {
        portx = atoi(argv[2]);
        if (portx > UINT16_MAX || portx < 0) {
            notify("Port falls outside of the valid range");
            return;
        }
    }

launch:
    child_spawned = 1;
    pid_t child = fork();
    if (child == 0) {
        char* args;
        asprintf(&args,
                 "exec %s --name %s --host %s --port %" PRIu16
                 " 2>&1 | cat >"
                 "/dev/null",
                 tab->exec_cmd, tab->username, ip, (uint16_t)portx);

        execve("/bin/sh",
               (char*[]){
                   "sh",
                   "-c",
                   args,
                   NULL,
               },
               environ);
        exit(EXIT_SUCCESS);
    } else if (child == -1) {
        child_spawned = 0;
        notify("Failed to launch the game process: %s", strerror(errno));
        return;
    }

    cfg->game_proc = child;
    notify("Launched game process: %jd", (intmax_t)child);
}

void set_fixedstr(void* target, const char* setting, void* params) {
    struct set_bounds* cfg = params;

    size_t len = strlen(setting);
    if (len < cfg->lower.usize || len > cfg->upper.usize) {
        notify(
            "The entered value's length falls out of the allowed range of "
            "[%zu, %zu]",
            cfg->lower.usize, cfg->upper.usize);
        return;
    }

    strcpy(target, setting);
}

void set_charp(void* target_, const char* setting, void* params) {
    (void)params;
    if (target_ == NULL) return;

    char** target = target_;

    if (*target != NULL) {
        free(*target);
        *target = NULL;
    }

    if (setting != NULL && *setting != '\0') *target = strdup(setting);
}

void set_num(void* target, const char* setting, void* params) {
    if (params == NULL || setting == NULL || target == NULL || *setting == '\0')
        return;
    struct set_num_params* cfg = params;

    errno = 0;
    long num = strtol(setting, (char**)NULL, 10);
    if (errno != 0) {
        notify("Failed to convert number.");
        return;
    }

    if (num < cfg->bounds.lower.ilong || num > cfg->bounds.upper.ilong) {
        notify(
            "The entered value falls out of the allowed range of "
            "[%ld, %ld]",
            cfg->bounds.lower.ilong, cfg->bounds.upper.ilong);
        return;
    }

    switch (cfg->num_type) {
        case SET_NUM_U8:
            *(uint8_t*)target = num;
            break;
        default:
            notify("Error");
            return;
    }
}

void set_filter(void* target, const char* setting, void* params) {
    (void)params;
    if (target == NULL || setting == NULL) {
        return;
    }

    uint8_t* filter = target;

    if (strcmp(setting, "full") == 0) {
        *filter ^= HIDE_FULL;
    } else if (strcmp(setting, "empty") == 0) {
        *filter ^= HIDE_EMPTY;
    } else if (strcmp(setting, "password") == 0) {
        *filter ^= HIDE_PR;
    } else {
        notify(
            "Unrecognized option \"%s\". Available options: full, empty, "
            "password",
            setting);
    }

    // sort_serverlist(, cfg.sort, *filter, buf);
}

void set_sort(void* target, const char* setting, void* params) {
    (void)params;
    if (target == NULL || setting == NULL || *setting == '\0') {
        return;
    }

    int8_t* sort = target;
    int8_t new_sort = params == NULL ? 0 : *sort;

    if (*setting == '-') {
        new_sort |= INT8_MIN;
        setting++;
    }

    if (strcmp(setting, "password") == 0) {
        new_sort ^= FIELD_PR;
    } else if (strcmp(setting, "name") == 0) {
        new_sort ^= FIELD_NAME;
    } else if (strcmp(setting, "players") == 0) {
        new_sort ^= FIELD_PC;
    } else if (strcmp(setting, "gamemode") == 0) {
        new_sort ^= FIELD_GM;
    } else if (strcmp(setting, "language") == 0) {
        new_sort ^= FIELD_LN;
    } else {
        notify(
            "Unrecognized option \"%s\". Available options: [-](password, "
            "name, players, gamemode, language)",
            setting);
        return;
    }

    *sort = new_sort;
}

static void call_set(const char** argv, void* cfg_) {
    struct tab_state* cfg = cfg_;
    if (argv[1] == NULL || argv[2] == NULL) {
        notify("Must provide 2 arguments to set");
        return;
    }

    for (size_t map = 0; map < cfg->map_count; map++) {
        struct setmap* curr = cfg->map + map;
        if (strcmp(argv[1], curr->name) == 0) {
            curr->setfunc(curr->target, argv[2], curr->params);
            return;
        }
    }
    notify("%s isn't a valid setting!", argv[1]);
}

static void call_source(const char** argv, void* cfg_) {
    struct app_state* cfg = cfg_;

    if (argv[1] == NULL || *argv[1] == '\0') {
        notify("Must specify rc file");
        return;
    }

    char path[PATH_MAX + 1]; /* this is the most hacky-feeling relatively
                                       normal thing to do */
    path[0] = '\0';

    if (*argv[1] == '~') {
        const char* home_path = getenv("HOME");
        if (home_path == NULL) {
            notify("HOME not set, can't expand '~'");
            return;
        }

        if (strlen(home_path) + strlen(argv[1]) - 1 > PATH_MAX) {
            notify("path length exceeds PATH_MAX");
            return;
        }

        strcat(path, home_path);
        strcat(path, argv[1] + 1);
    } else {
        if (strlen(argv[1]) > PATH_MAX) {
            notify("path length exceeds PATH_MAX");
            return;
        }

        strcat(path, argv[1]);
    }

    FILE* f = fopen(path, "r");
    if (f == NULL) {
        notify("Failed to open the file: %s", strerror(errno));
        return;
    }

    char* cmd = NULL;
    size_t length = 0;
    while (getline(&cmd, &length, f) > 0) {
        cmd[strlen(cmd) - 1] =
            '\0'; /* trim the newline at the end that getline leaves */
        handle_cmd(cmd, cfg);
    }

    if (ferror(f) != 0) {
        notify("An error happened during processing of the file: %s",
               strerror(errno));
    }

    fclose(f);
    free(cmd);
}

static void call_fetch(const char** argv, void* cfg_) {
    struct tab_state* cfg = cfg_;

    const char* remote = "https://api.open.mp/servers";
    if (argv[1] == NULL) {
        notify("No api url provided! Assuming \"%s\"", remote);
    } else {
        remote = argv[1];
    }

    struct servlist* new_list = fetch_servers(remote);
    if (new_list == NULL) {
        notify("Failed to fetch server list.");
        return;
    }

    servlist_free(cfg->list);
    cfg->list = new_list;
}

static void call_add(const char** argv, void* cfg_) {
    struct tab_state* tab = cfg_;

    const char* generic_err =
        "Address wasn't specified properly! Expected :add <addr> <port>";

    if (argv[1] == NULL) {
        notify(generic_err);
        return;
    }

    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_protocol = SOCK_DGRAM,
    };

    struct addrinfo* resp;

    int ret = getaddrinfo(argv[1], NULL, &hints, &resp);
    if (ret != 0) {
        notify("Failed to resolve address: %s", gai_strerror(ret));
        return;
    }
    if (resp->ai_addr->sa_family != AF_INET) {
        notify("Failed to resolve address");
        freeaddrinfo(resp);
        return;
    }
    struct sockaddr_in addr = *(struct sockaddr_in*)(resp->ai_addr);
    freeaddrinfo(resp);

    int portx;
    if (argv[2] == NULL) {
        portx = 7777;
    } else {
        portx = atoi(argv[2]);
        if (portx > UINT16_MAX || portx < 0) {
            notify("Port falls outside of the valid range");
            return;
        }
    }

    addr.sin_port = htons((uint16_t)portx);

    // clang-format off
    if (tab->list == NULL || tab->list->len >= tab->list->cap)
        if (servlist_resize(&tab->list, 
                tab->list != NULL ? tab->list->cap + tab->list->cap / 2 : 2) < 0) {
            notify("Failed to allocate");
            return;
        }
    // clang-format on

    ret = servquery_info(addr, tab->list->servs + tab->list->len);
    if (ret < 0) {
        notify("Failed to query server: %d", ret);
        return;
    }
    tab->list->len++;
}

#define LVL_APPLICATION 1
#define LVL_TAB 0

static const struct regcmd {
    const char* cmd;
    void (*call)(const char**, void*);
    size_t expected_args;
    uint8_t lvl;
} commands[] = {
    {
        .cmd = "q",
        .call = call_quit,
        .expected_args = 0,
        .lvl = LVL_APPLICATION,
    },
    {
        .cmd = "echo",
        .call = call_echo,
        .expected_args = 2,
    },
    {
        .cmd = "connect",
        .call = call_connect,
        .expected_args = 3,
        .lvl = LVL_APPLICATION, /* required because child pid is
                                   application-global */
    },
    {
        .cmd = "set",
        .call = call_set,
        .expected_args = 3,
        /* .lvl = LVL_APPLICATION */ /* will probably be required unless i
                                        figure out something better. */
    },
    {
        .cmd = "so",
        .call = call_source,
        .expected_args = 2,
        .lvl = LVL_APPLICATION,
    },
    {
        .cmd = "fetch",
        .call = call_fetch,
        .expected_args = 2,
    },
    {
        .cmd = "add",
        .call = call_add,
        .expected_args = 3,
    },
};

#define CMD_TOKS 3
#if CMD_TOKS <= 1 || CMD_TOKS == SIZE_MAX
#error Definition of CMD_TOKS falls out of the valid range [2, SIZE_MAX)
#endif
void handle_cmd(char* cmd, struct app_state* cfg) {
    if (cmd == NULL || cfg == NULL) return;

    char* toks[CMD_TOKS + 1] = {0};
    size_t parsed_toks = 0;

    char* curr_tok = cmd;
    char* cursor = cmd;
    do {
        toks[parsed_toks] = curr_tok;

        while (*cursor != '\0') {
            if (*cursor == ' ' && parsed_toks < CMD_TOKS - 1) {
                *cursor = '\0';
                curr_tok = ++cursor;
                break;
            }
            cursor++;
        }
        if (curr_tok == toks[parsed_toks++]) break;
    } while (parsed_toks < CMD_TOKS);

    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(toks[0], commands[i].cmd) == 0) {
            /* concatenate excess tokens */
            if (parsed_toks > commands[i].expected_args &&
                commands[i].expected_args > 1) {
                for (size_t toki = commands[i].expected_args - 1;
                     toki < parsed_toks - 1; toki++) {
                    toks[toki][strlen(toks[toki])] = ' ';
                }
                toks[commands[i].expected_args] = NULL;
            }

            void* arg;
            switch (commands[i].lvl) {
                case LVL_TAB:
                    arg = cfg->tabs + cfg->tabs_selected;
                    break;
                case LVL_APPLICATION:
                    arg = cfg;
                    break;
                default:
                    arg = NULL;
            }

            commands[i].call((const char**)toks, arg);
        }
    }
}
