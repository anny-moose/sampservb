#include "cmd.h"

#include <arpa/inet.h>
#include <curses.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "list.h"
#include "serv.h"
#include "servfetch.h"
#include "states.h"

extern char** environ;

struct regcmd {
    const char* cmd;
    sidefx (*call)(const char**, void*);
    size_t expected_args;
    uint8_t lvl;
};

static const struct regcmd* commands;
static const size_t cmd_count;

struct keymapping {
    sidefx (*call)(const char**, void*);
    uint8_t lvl;
    char* argv[];
};

#define CMD_TOKS 3
#if CMD_TOKS <= 1 || CMD_TOKS == SIZE_MAX
#error Definition of CMD_TOKS falls out of the valid range [2, SIZE_MAX)
#endif
static size_t parse_toks(char* input, char** output, size_t ntoks) {
    if (ntoks < 1 || input == NULL) return 0;

    size_t parsed_toks = 0;

    char* curr_tok = input;
    char* cursor = input;
    do {
        output[parsed_toks] = curr_tok;

        while (*cursor != '\0') {
            if (*cursor == ' ' && parsed_toks < ntoks - 1) {
                *cursor = '\0';
                curr_tok = ++cursor;
                break;
            }
            cursor++;
        }
        if (curr_tok == output[parsed_toks++]) break;
    } while (parsed_toks < ntoks);

    return parsed_toks;
}

static sidefx call_quit(const char** argv, void* cfg_) {
    (void)argv;
    struct app_state* cfg = cfg_;

    cfg->quit = true;
    return 0;
}

static sidefx call_echo(const char** argv, void* cfg_) {
    (void)cfg_;

    notify("%s", argv[1]);
    return 0;
}

static sidefx call_connect(const char** argv, void* cfg_) {
    struct app_state* cfg = cfg_;
    struct tab_state* tab = cfg->tabs + cfg->tabs_selected;
    if (tab->exec_cmd == NULL) {
        notify("exec_cmd Isn't set!");
        return 0;
    }

    if (*tab->username == '\0') {
        notify("username Isn't set!");
        return 0;
    }

    if (child_spawned != 0) {
        notify("Game process already exists: %jd", (intmax_t)cfg->game_proc);
        return 0;
    }

    const char* generic_err =
        "Address wasn't specified properly! Expected :connect <addr> <port>";

    char ip[INET_ADDRSTRLEN];
    int portx;

    if (argv[1] == NULL) {
        if (tab->list == NULL || tab->selected >= tab->list->num_displayed) {
            notify(generic_err);
            return 0;
        }

        const struct servinfo* serv = tab->list->servs + tab->selected;

        inet_ntop(AF_INET, &serv->ip, ip, INET_ADDRSTRLEN);
        portx = ntohs(serv->port);
        goto launch;
    }

    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_protocol = IPPROTO_UDP,
        .ai_socktype = SOCK_DGRAM,
    };

    struct addrinfo* resp;

    int ret = getaddrinfo(argv[1], NULL, &hints, &resp);
    if (ret != 0) {
        notify("Failed to resolve address: %s", gai_strerror(ret));
        return 0;
    }
    if (resp->ai_addr->sa_family != AF_INET) {
        notify("Failed to resolve address");
        freeaddrinfo(resp);
        return 0;
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
            return 0;
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
        return 0;
    }

    cfg->game_proc = child;
    notify("Launched game process: %jd", (intmax_t)child);
    return 0;
}

static void set_fixedstr(void* target, const char* setting,
                         const void* params) {
    const struct set_bounds* cfg = params;

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

static void set_charp(void* target_, const char* setting, const void* params) {
    (void)params;
    if (target_ == NULL) return;

    char** target = target_;

    if (*target != NULL) {
        free(*target);
        *target = NULL;
    }

    if (setting != NULL && *setting != '\0') *target = strdup(setting);
}

/* Temorarily commented out due to compiler warning */
/*
static void set_num(void* target, const char* setting, const void* params) {
    if (params == NULL || setting == NULL || target == NULL || *setting == '\0')
        return;
    const struct set_num_params* cfg = params;

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
*/

static void set_filter(void* target, const char* setting, const void* params) {
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
}

static void set_sort(void* target, const char* setting, const void* params) {
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

const static struct set_bounds uname = {
    .lower.usize = 3,
    .upper.usize = 24,
};

const static struct set_bounds tabname = {
    .lower.usize = 0,
    .upper.usize = TABNAME_LEN - 1,
};

const static struct setmap maps[] = {
    {
        .name = "exec_cmd",
        .offset = offsetof(struct tab_state, exec_cmd),
        .setfunc = set_charp,
    },
    {
        .name = "tabname",
        .offset = offsetof(struct tab_state, tab_name),
        .setfunc = set_fixedstr,
        .fx = REFRESH_TAB,
        .params = &tabname,
    },
    {
        .name = "username",
        .offset = offsetof(struct tab_state, username),
        .setfunc = set_fixedstr,
        .params = &uname,
    },
    {
        .name = "filter",
        .offset = offsetof(struct tab_state, filters),
        .setfunc = set_filter,
        .fx = REFRESH_SORT | REFRESH_LIST,
    },
    {
        .name = "shown",
        .offset = offsetof(struct tab_state, display) +
                  offsetof(struct display_cfg, shown_fields),
        .setfunc = set_sort,
        .params = (void*)1,
        .fx = REFRESH_LIST,
    },
    {
        .name = "sort",
        .offset = offsetof(struct tab_state, sort),
        .setfunc = set_sort,
        .fx = REFRESH_SORT | REFRESH_LIST,
    },
};

static sidefx call_set(const char** argv, void* cfg_) {
    struct tab_state* cfg = cfg_;
    if (argv[1] == NULL || argv[2] == NULL) {
        notify("Must provide 2 arguments to set");
        return 0;
    }

    for (size_t map = 0; map < sizeof(maps) / sizeof(maps[0]); map++) {
        const struct setmap* curr = maps + map;
        if (strcmp(argv[1], curr->name) == 0) {
            curr->setfunc(((char*)cfg) + curr->offset, argv[2], curr->params);
            return curr->fx;
        }
    }

    notify("%s isn't a valid setting!", argv[1]);
    return 0;
}

static sidefx call_source(const char** argv, void* cfg_) {
    struct app_state* cfg = cfg_;
    sidefx ret = 0;

    if (argv[1] == NULL || *argv[1] == '\0') {
        notify("Must specify rc file");
        return ret;
    }

    char path[PATH_MAX + 1]; /* this is the most hacky-feeling relatively
                                       normal thing to do */
    path[0] = '\0';

    if (*argv[1] == '~') {
        const char* home_path = getenv("HOME");
        if (home_path == NULL) {
            notify("HOME not set, can't expand '~'");
            return ret;
        }

        if (strlen(home_path) + strlen(argv[1]) - 1 > PATH_MAX) {
            notify("path length exceeds PATH_MAX");
            return ret;
        }

        strcat(path, home_path);
        strcat(path, argv[1] + 1);
    } else {
        if (strlen(argv[1]) > PATH_MAX) {
            notify("path length exceeds PATH_MAX");
            return ret;
        }

        strcat(path, argv[1]);
    }

    FILE* f = fopen(path, "r");
    if (f == NULL) {
        notify("Failed to open the file: %s", strerror(errno));
        return ret;
    }

    char* cmd = NULL;
    size_t length = 0;

    struct tab_state* tab = cfg->tabs + cfg->tabs_selected;
    while (getline(&cmd, &length, f) > 0) {
        cmd[strlen(cmd) - 1] =
            '\0'; /* trim the newline at the end that getline leaves */
        ret |= handle_cmd(cmd, cfg);
        if (ret & REFRESH_TAB) tab = cfg->tabs + cfg->tabs_selected;

        if (ret & REFRESH_SORT) {
            ret ^= REFRESH_SORT;
            sort_serverlist(tab->list, tab->sort, tab->filters,
                            tab->search_buf);
        }
    }

    if (ferror(f) != 0) {
        notify("An error happened during processing of the file: %s",
               strerror(errno));
    }

    fclose(f);
    free(cmd);
    return ret;
}

static sidefx call_fetch(const char** argv, void* cfg_) {
    struct tab_state* cfg = cfg_;

    const char* remote = "https://api.open.mp/servers";
    if (argv[1] == NULL) {
        notify("No api url provided! Assuming \"%s\"", remote);
    } else {
        remote = argv[1];
    }

    char* params = NULL;
    struct json_keys keys = {
        .pa_key = "pa",
        .ip_key = "ip",
        .pc_key = "pc",
        .pm_key = "pm",
        .string_keys = {"hn", "gm", "la"},
    };
    if (argv[2] != NULL) {
        params = strdup(argv[2]);
        if (params == NULL) {
            notify("Failed to allocate memory: %s", strerror(errno));
            return 0;
        }

        char* param = params;
        /* this is like. really ugly */
#define GETTOK(dst)             \
    (dst) = param;              \
    param = strchr(param, ';'); \
    if (param == NULL) break;   \
    *param = '\0';              \
    param++;
        do {
            GETTOK(keys.pa_key);
            GETTOK(keys.ip_key);
            GETTOK(keys.pc_key);
            GETTOK(keys.pm_key);

            for (size_t i = 0; i < 3; i++) {
                GETTOK(keys.string_keys[i]);
            }
        } while (0);
    }

    struct servlist* new_list = fetch_servers(remote, keys);
    free(params);
    if (new_list == NULL) {
        notify("Failed to fetch server list.");
        return 0;
    }

    servlist_free(cfg->list);
    cfg->list = new_list;
    return REFRESH_SORT | REFRESH_LIST;
}

static sidefx call_add(const char** argv, void* cfg_) {
    struct tab_state* tab = cfg_;

    const char* generic_err =
        "Address wasn't specified properly! Expected :add <addr> <port>";

    if (argv[1] == NULL) {
        notify(generic_err);
        return 0;
    }

    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_protocol = IPPROTO_UDP,
        .ai_socktype = SOCK_DGRAM,
    };

    struct addrinfo* resp;

    int ret = getaddrinfo(argv[1], NULL, &hints, &resp);
    if (ret != 0) {
        notify("Failed to resolve address: %s", gai_strerror(ret));
        return 0;
    }
    if (resp->ai_addr->sa_family != AF_INET) {
        notify("Failed to resolve address");
        freeaddrinfo(resp);
        return 0;
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
            return 0;
        }
    }

    addr.sin_port = htons((uint16_t)portx);

    // clang-format off
    if (tab->list == NULL || tab->list->len >= tab->list->cap)
        if (servlist_resize(&tab->list, 
                tab->list != NULL ? tab->list->cap + tab->list->cap / 2 : 2) < 0) {
            notify("Failed to allocate");
            return 0;
        }
    // clang-format on

    ret = servquery_info(addr, tab->list->servs + tab->list->len);
    if (ret < 0) {
        notify("Failed to query server: %d", ret);
        return 0;
    }
    tab->list->len++;

    return REFRESH_SORT | REFRESH_LIST;
}

static sidefx call_refetch(const char** argv, void* cfg_) {
    (void)argv;
    struct tab_state* tab = cfg_;

    if (tab->list == NULL || tab->selected >= tab->list->num_displayed) {
        notify("Select a server to refresh");
        return 0;
    }

    struct servinfo* serv = tab->list->servs + tab->selected;

    struct sockaddr_in addr = (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_addr = serv->ip,
        .sin_port = serv->port,
    };

    free(serv->txt);
    int ret = servquery_info(addr, serv);
    if (ret < 0) {
        notify("Failed to query server: %d", ret);
        return 0;
    }

    return REFRESH_LIST;
}

static void writestr_rules(WINDOW* win, const void* data, unsigned char col,
                           size_t elem, int xleft, void* userdata) {
    struct servrules* list = userdata;
    if (list == NULL) return;
    const struct servrule* entry = data;
    entry += elem;

    switch (col) {
        case 0:
            waddnstr(win, list->txt + entry->name_off, xleft);
            break;
        case 1:
            waddnstr(win, list->txt + entry->value_off, xleft);
            break;
        default:
            break;
    }
}

static sidefx call_rules(const char** argv, void* cfg_) {
    (void)argv;
    struct tab_state* tab = cfg_;

    if (tab->list == NULL || tab->selected >= tab->list->num_displayed) {
        notify("Select a server to refresh");
        return 0;
    }

    struct servinfo* serv = tab->list->servs + tab->selected;

    struct sockaddr_in addr = (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_addr = serv->ip,
        .sin_port = serv->port,
    };

    struct servrules* rules;

    int ret = servquery_rules(addr, &rules);
    if (ret < 0) {
        notify("Failed to query server: %d", ret);
        return 0;
    }

    int winypos = LINES / 6;
    int winxpos = COLS / 6;
    int winheight = winypos * 4;
    int winwidth = winxpos * 4;

    struct coldesc cols[2] = {
        {"Variable", winwidth / 2},
        {"Value", winwidth / 2},
    };

    struct listdesc ldec = {
        .ncols = 2,
        .cols = cols,
        .userdata = rules,
        .writestr = writestr_rules,
        .opt = LIST_OPT_BORDER,
    };

    WINDOW* notif = newwin(winheight, winwidth, winypos, winxpos);

    draw_list(notif, &ldec, rules->rules, rules->len, 0, 0);
    int a = wgetch(notif);
    ungetch(a);

    delwin(notif);

    free(rules->txt);
    free(rules);

    return REFRESH_LIST;
}

static sidefx call_tabnew(const char** argv, void* cfg_) {
    struct app_state* cfg = cfg_;
    (void)argv;

    if (cfg->tabs_count >= cfg->tabs_capacity) {
        size_t new_cap = cfg->tabs_capacity + 1;
        struct tab_state* new_tabs =
            realloc(cfg->tabs, sizeof(*cfg->tabs) * new_cap);
        if (new_tabs == NULL) {
            notify("Failed to allocate!");
            return 0;
        }
        cfg->tabs = new_tabs;
        cfg->tabs_capacity = new_cap;
    }

    if (init_tab(&cfg->tabs[cfg->tabs_count]) < 0) {
        notify("Failed to create tab!");
        return 0;
    }

    cfg->tabs_selected = cfg->tabs_count;
    cfg->tabs_count++;

    snprintf(cfg->tabs[cfg->tabs_selected].tab_name, TABNAME_LEN, "Tab %zu",
             cfg->tabs_count);

    return REFRESH_TAB | REFRESH_LIST;
}

static sidefx call_tabmove(const char** argv, void* cfg_) {
    struct app_state* cfg = cfg_;

    int change = 1;
    if (argv[1] != NULL) change = abs(atoi(argv[1]));

    if (strcmp(argv[0], "tabnext") == 0) {
        cfg->tabs_selected =
            MIN(cfg->tabs_count - 1, cfg->tabs_selected + change);
    } else {
        if ((size_t)change > cfg->tabs_selected)
            cfg->tabs_selected = 0;
        else
            cfg->tabs_selected -= change;
    }

    return REFRESH_TAB | REFRESH_LIST;
}

static sidefx call_move(const char** argv, void* cfg_) {
    struct tab_state* tab = cfg_;

    if (tab->list == NULL) return 0;

    int change = 1;
    if (argv[1] != NULL) change = abs(atoi(argv[1]));

    if (strcmp(argv[0], "next") == 0) {
        tab->selected =
            MIN(tab->list->num_displayed - 1, tab->selected + change);
    } else {
        if ((size_t)change > tab->selected)
            tab->selected = 0;
        else
            tab->selected -= change;
    }

    return REFRESH_LIST;
}

static sidefx call_sortmove(const char** argv, void* cfg_) {
    struct tab_state* tab = cfg_;

    const uint8_t shown = tab->display.shown_fields;
    uint8_t sort = tab->visual_sort;

    if (strcmp(argv[0], "sortnext") == 0) {
        while (sort != 0) {
            sort <<= 1;
            if ((sort & shown) && sort <= FIELD_LN) break;
        }
    } else {
        while (sort != 0) {
            sort >>= 1;
            if (sort & shown) break;
        }
    }

    if (sort == 0) return 0;

    tab->visual_sort = sort;
    return REFRESH_LIST;
}

static sidefx call_sort(const char** argv, void* cfg_) {
    struct tab_state* tab = cfg_;
    (void)argv;

    if (tab->sort == tab->visual_sort)
        tab->sort |= (int8_t)INT8_MIN;
    else
        tab->sort = tab->visual_sort;

    return REFRESH_SORT | REFRESH_LIST;
}

static sidefx call_remap(const char** argv, void* cfg_) {
    struct app_state* cfg = cfg_;

    size_t idx;

    if (argv[1] == NULL) {
        notify("Must specify a key to map to.");
        return 0;
    }

    if (*argv[1] == '\0') {
        idx = ' ' - 32;
    } else if (argv[1][1] != '\0') {
        if (strcmp(argv[1], "<CR>") == 0) {
            idx = ':' - 32;
        } else if (strcmp(argv[1], "<Tab>") == 0) {
            idx = '/' - 32;
        } else {
            notify("Must specify a key to map to, or <CR>/<Tab>");
            return 0;
        }
    } else {
        if (*argv[1] < 32 || *argv[1] > 127 || *argv[1] == ':' ||
            *argv[1] == '\\') {
            notify("Can't map to this key");
            return 0;
        }

        idx = *argv[1] - 32;
    }

    if (argv[2] == NULL) {
        free(cfg->keycmd[idx]);
        cfg->keycmd[idx] = NULL;
        return 0;
    }

    size_t alloc_size = sizeof(struct keymapping);
    size_t argc = 0;
    uint8_t lvl = 0;
    sidefx (*func)(const char**, void*) = NULL;

    for (size_t i = 0; i < cmd_count; i++) {
        if (strcmp(argv[2], commands[i].cmd) == 0) {
            argc = commands[i].expected_args;
            func = commands[i].call;
            lvl = commands[i].lvl;
            break;
        }
    }

    if (func == NULL) {
        notify("Couldn't find function: %s", argv[2]);
        return 0;
    }

    if (argc != 0) {
        alloc_size += sizeof(char*) * argc;
        alloc_size += strlen(argv[2]) + 1;
        if (argv[3] != NULL) {
            alloc_size += strlen(argv[3]) + 1;
        }
    }

    struct keymapping* newmap = calloc(1, alloc_size);
    if (newmap == NULL) {
        notify("Failed to allocate!");
        return 0;
    }

    newmap->call = func;
    newmap->lvl = lvl;
    if (argc != 0) {
        char* args = stpcpy((char*)(newmap->argv + argc), argv[2]) + 1;
        newmap->argv[0] = (char*)(newmap->argv + argc);
        if (argv[3] != NULL) {
            stpcpy(args, argv[3]);
            parse_toks(args, newmap->argv + 1, argc - 1);
        }
    }

    free(cfg->keycmd[idx]);
    cfg->keycmd[idx] = newmap;

    return 0;
}

static sidefx call_tab(const char** argv, void* cfg_) {
    struct app_state* cfg = cfg_;

    if (argv[1] == NULL) {
        return 0;
    }

    int tab = atoi(argv[1]) - 1;
    if (tab < 0 || (size_t)tab >= cfg->tabs_count) {
        notify("Invalid tab specified. Must be a number between 1 and %zu",
               cfg->tabs_count);
        return 0;
    }

    cfg->tabs_selected = tab;
    return REFRESH_TAB | REFRESH_LIST;
}

#define LVL_APPLICATION 1
#define LVL_TAB 0

static const struct regcmd cmd_arr[] = {
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
        .expected_args = 3,
    },
    {
        .cmd = "refetch",
        .call = call_refetch,
        .expected_args = 0,
    },
    {
        .cmd = "rules",
        .call = call_rules,
        .expected_args = 0,
    },
    {
        .cmd = "add",
        .call = call_add,
        .expected_args = 3,
    },
    {
        .cmd = "tabnew",
        .call = call_tabnew,
        .expected_args = 0,
        .lvl = LVL_APPLICATION,
    },
    {
        .cmd = "tabnext",
        .call = call_tabmove,
        .expected_args = 2,
        .lvl = LVL_APPLICATION,
    },
    {
        .cmd = "tabprev",
        .call = call_tabmove,
        .expected_args = 2,
        .lvl = LVL_APPLICATION,
    },
    {
        .cmd = "tab",
        .call = call_tab,
        .expected_args = 2,
        .lvl = LVL_APPLICATION,
    },
    {
        .cmd = "next",
        .call = call_move,
        .expected_args = 2,
    },
    {
        .cmd = "prev",
        .call = call_move,
        .expected_args = 2,
    },
    {
        .cmd = "sortnext",
        .call = call_sortmove,
        .expected_args = 2,
    },
    {
        .cmd = "sortprev",
        .call = call_sortmove,
        .expected_args = 2,
    },
    {
        .cmd = "map",
        .call = call_remap,
        .expected_args = 4,
        .lvl = LVL_APPLICATION,
    },
    {
        .cmd = "sort",
        .call = call_sort,
        .expected_args = 0,
    },
};
static const struct regcmd* commands = cmd_arr;
static const size_t cmd_count = sizeof(cmd_arr) / sizeof(cmd_arr[0]);

sidefx handle_cmd(char* cmd, struct app_state* cfg) {
    if (cmd == NULL || cfg == NULL) return 0;

    char* toks[CMD_TOKS + 1] = {0};
    size_t parsed_toks = parse_toks(cmd, toks, 2);
    if (parsed_toks < 1) return 0;

    for (size_t i = 0; i < cmd_count; i++) {
        if (strcmp(toks[0], commands[i].cmd) == 0) {
            if (commands[i].expected_args > CMD_TOKS + 1) {
                abort();
                return 0;
            }

            parsed_toks +=
                parse_toks(toks[1], toks + 1, commands[i].expected_args);

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

            return commands[i].call((const char**)toks, arg);
        }
    }

    return 0;
}

sidefx keymapping_call(struct keymapping* map, struct app_state* cfg) {
    if (map == NULL) return 0;

    void* arg;
    switch (map->lvl) {
        case LVL_TAB:
            arg = cfg->tabs + cfg->tabs_selected;
            break;
        case LVL_APPLICATION:
            arg = cfg;
            break;
        default:
            arg = NULL;
    }

    return map->call((const char**)map->argv, arg);
}
