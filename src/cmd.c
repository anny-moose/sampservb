#include "cmd.h"

#include <arpa/inet.h>
#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "keymap.h"
#include "list.h"
#include "serv.h"
#include "servfetch.h"
#include "states.h"

extern char** environ;

#define CMD_TOKS 3
#if CMD_TOKS <= 1 || CMD_TOKS == SIZE_MAX
#error Definition of CMD_TOKS falls out of the valid range [2, SIZE_MAX)
#endif
size_t parse_toks(char* input, char** output, size_t ntoks) {
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

static int parse_path(const char* path, char out[PATH_MAX]) {
    if (*path == '\0') return -1;
    size_t path_len = strlen(path);
    size_t pfx_len = 0;

    if (path_len >= PATH_MAX) return -1;

    if (path[0] == '~' && path[1] == '/') {
        const char* home_path = getenv("HOME");
        if (home_path == NULL) {
            return -2;
        }

        pfx_len = strlen(home_path);

        if (pfx_len > PATH_MAX - path_len) {
            return -1;
        }

        memcpy(out, home_path, pfx_len);
        path_len--;
        path++;
    }

    memcpy(out + pfx_len, path, path_len);
    out[path_len + pfx_len] = '\0';

    return 0;
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
    struct servinfo serv;

    if (argv[1] == NULL) {
        if (tab->list == NULL || tab->selected >= tab->list->num_displayed) {
            notify(generic_err);
            return 0;
        }

        serv = tab->list->servs[tab->selected];

        inet_ntop(AF_INET, &serv.ip, ip, INET_ADDRSTRLEN);
        portx = ntohs(serv.port);
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
    inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);

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

    /* info is queried to check whether server is password-protected and prompt
     * the user if so. */
    ret = servquery_info(addr, &serv, true);
    if (ret < 0) {
        notify("Failed to query server info: %s", strerror(-ret));
        return 0;
    }

    char buf[64];
launch:
    if (serv.pa == true) {
        getinput("Enter password: ", buf, sizeof(buf));
    }

    child_spawned = 1;
    pid_t child = fork();
    if (child == 0) {
        char txtport[7];
        int pipefd[2];

        snprintf(txtport, sizeof(txtport), "%" PRIu16, (uint16_t)portx);

        // clang-format off
        char* cargv[10] = {
            tab->exec_cmd,
            "--name", tab->username,
            "--host", ip,
            "--port", txtport,
            (serv.pa == true) ? "--password" : NULL, buf,
            NULL,
        };
        // clang-format on

        /* the pipe (and later grandchild) are used in order to keep track of
         * the whole lineage of the game process. as windows doesn't have an
         * execve-style interface, it is required in the case of e.g spawning
         * another */
        if (pipe(pipefd) < 0) {
            exit(EXIT_FAILURE);
        }

        stdscr = NULL; /* this is by all means a hack to prevent this child from
                          invoking notify() inside of it's sigchld handler,
                          which is inherited from the parent. */

        pid_t gchild = fork();
        if (gchild == 0) {
            int wrnull = open("/dev/null", O_WRONLY);
            int rdnull = open("/dev/null", O_RDONLY);
            close(pipefd[0]);

            if (wrnull < 0 || rdnull < 0) exit(EXIT_FAILURE);

            /* unfortunately, it appears that wine (or bottles-cli, which was
             * used during testing) closes any extra file descriptors upon
             * forking off and creating the game process, meaning that
             * stdout/stderr has to be replaced, and input will have to be
             * manually discarded in child */
            if (dup2(rdnull, STDIN_FILENO) < 0
                || dup2(pipefd[1], STDOUT_FILENO) < 0
                || dup2(wrnull, STDERR_FILENO) < 0) {
                exit(EXIT_FAILURE);
            }

            close(wrnull);
            close(rdnull);
            close(pipefd[1]);

            if (execve(tab->exec_cmd, cargv, environ) < 0) {
                exit(EXIT_FAILURE);
            }
        } else if (gchild == -1) {
            exit(EXIT_FAILURE);
        }

        close(pipefd[1]);
        ssize_t read_bytes;
        /* use the largest buffer that isn't reused */
        while ((read_bytes = read(pipefd[0], buf, sizeof(buf))) != 0) {
            if (read_bytes < 0) {
                if (errno != EINTR) exit(EXIT_FAILURE);
            }
        }

        close(pipefd[0]);

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
    if (target_ == NULL) return;

    char** target = target_;

    char path[PATH_MAX];
    if (params != NULL) {
        int res = parse_path(setting, path);
        if (res < 0) {
            notify("Failed to parse path: %s",
                   res == -1 ? "Invalid input" : "Error expanding HOME");
            return;
        }
    }

    if (*target != NULL) {
        free(*target);
        *target = NULL;
    }

    if (setting != NULL && *setting != '\0') {
        *target = strdup(params == NULL ? setting : path);
        if (*target == NULL) {
            notify("Failed to allocate memory!");
        }
    }
}

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
            if (num < 0 || num > UINT8_MAX) return;
            *(uint8_t*)target = num;
            break;
        default:
            notify("Error");
            return;
    }
}

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

const static struct set_num_params setu8_params = {
    .bounds =
        {
            .lower.ilong = 0,
            .upper.ilong = UINT8_MAX,
        },
    .num_type = SET_NUM_U8,
};

const static struct setmap maps[] = {
    {
        .name = "exec_cmd",
        .offset = offsetof(struct tab_state, exec_cmd),
        .setfunc = set_charp,
        .params = (void*)1,
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
        .offset = offsetof(struct tab_state, display)
                  + offsetof(struct display_cfg, shown_fields),
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
    {
        .name = "namecols",
        .offset = offsetof(struct tab_state, display)
                  + offsetof(struct display_cfg, name_cols),
        .setfunc = set_num,
        .params = &setu8_params,
        .fx = REFRESH_LIST,
    },
    {
        .name = "gmcols",
        .offset = offsetof(struct tab_state, display)
                  + offsetof(struct display_cfg, gm_cols),
        .setfunc = set_num,
        .params = &setu8_params,
        .fx = REFRESH_LIST,
    },
    {
        .name = "lncols",
        .offset = offsetof(struct tab_state, display)
                  + offsetof(struct display_cfg, ln_cols),
        .setfunc = set_num,
        .params = &setu8_params,
        .fx = REFRESH_LIST,
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

    char path[PATH_MAX];

    int res = parse_path(argv[1], path);
    if (res < 0) {
        notify("Failed to parse path: %s",
               res == -1 ? "Invalid input" : "Error expanding HOME");
        return ret;
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

static sidefx call_masterlist(const char** argv, void* cfg_) {
    struct tab_state* cfg = cfg_;

    const char* remote = "https://sam.markski.ar/api/GetMasterlist";
    if (argv[1] == NULL) {
        notify("No api url provided! Assuming \"%s\"", remote);
    } else {
        remote = argv[1];
    }

    struct servlist* new_list = fetch_masterlist(remote);
    if (new_list == NULL) {
        notify("Failed to fetch master list.");
        return 0;
    }

    servlist_free(cfg->list);
    cfg->list = new_list;
    return REFRESH_SORT | REFRESH_LIST;
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

    ret = servquery_info(addr, tab->list->servs + tab->list->len, false);
    if (ret < 0) {
        notify("Failed to query server: %s", strerror(-ret));
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
    int ret = servquery_info(addr, serv, false);
    if (ret < 0) {
        notify("Failed to query server: %s", strerror(-ret));
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

static void popout_list(struct listdesc* desc, const void* data, size_t nels) {
    int winypos = LINES / 6;
    int winxpos = COLS / 6;
    int winheight = winypos * 4;
    int winwidth = winxpos * 4;

    for (unsigned char col = 0; col < desc->ncols; col++) {
        desc->cols[col].width = winwidth / desc->ncols;
    }

    size_t select = 0;
    WINDOW* notif = newwin(winheight, winwidth, winypos, winxpos);

    notify("Press Enter to close popup");
    draw_list(notif, desc, data, nels, select, 0);

    int a;
    while ((a = wgetch(notif)) != '\n') {
        switch (a) {
            case 'j':
                if (select + 1 < nels) select++;
                break;
            case 'k':
                if (select > 0) select--;
                break;
            case ':':
                delwin(notif);
                ungetch(a);
                return;
            default:
                break;
        }
        draw_list(notif, desc, data, nels, select, 0);
    }

    delwin(notif);
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
        notify("Failed to query server: %s", strerror(-ret));
        return 0;
    }

    struct coldesc cols[2] = {
        {"Variable", 0},
        {"Value", 0},
    };

    struct listdesc ldesc = {
        .ncols = 2,
        .cols = cols,
        .userdata = rules,
        .writestr = writestr_rules,
        .opt = LIST_OPT_BORDER | LIST_OPT_SELSCR,
    };

    popout_list(&ldesc, rules->rules, rules->len);

    free(rules->txt);
    free(rules);

    return REFRESH_LIST;
}

static void writestr_clients(WINDOW* win, const void* data, unsigned char col,
                             size_t elem, int xleft, void* userdata) {
    struct servclients* list = userdata;
    if (list == NULL) return;
    const struct servclient* entry = data;
    entry += elem;

    /* ~4.2 billion max in a u32? */
    char buf[11];

    switch (col) {
        case 0:
            waddnstr(win, list->txt + entry->name_off, xleft);
            break;
        case 1:
            snprintf(buf, sizeof(buf), "%" PRIu32, entry->score);
            waddnstr(win, buf, xleft);
            break;
        default:
            break;
    }
}

static sidefx call_clients(const char** argv, void* cfg_) {
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

    struct servclients* clients;

    int ret = servquery_clients(addr, &clients);
    if (ret < 0) {
        notify("Failed to query server: %s", strerror(-ret));
        return 0;
    }

    struct coldesc cols[2] = {
        {"Name", 0},
        {"Score", 0},
    };

    struct listdesc ldesc = {
        .ncols = 2,
        .cols = cols,
        .userdata = clients,
        .writestr = writestr_clients,
        .opt = LIST_OPT_BORDER | LIST_OPT_SELSCR,
    };

    popout_list(&ldesc, clients->clients, clients->len);

    free(clients->txt);
    free(clients);

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

static sidefx call_tabdel(const char** argv, void* cfg_) {
    (void)argv;
    struct app_state* cfg = cfg_;
    struct tab_state* tab = cfg->tabs + cfg->tabs_selected;

    if (cfg->tabs_count < 1) {
        notify("Unexpected state");
        return 0;
    }

    free_tab(tab);

    if (cfg->tabs_selected + 1 < cfg->tabs_count) {
        const size_t nbytes = sizeof(struct tab_state)
                              * (cfg->tabs_count - 1 - cfg->tabs_selected);
        memmove(tab, tab + 1, nbytes);
    }

    if (cfg->tabs_count > 1) cfg->tabs_count--;

    if (cfg->tabs_selected > 0) cfg->tabs_selected--;

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

    int key = keymap_strtokey(argv[1]);
    if (key == ERR) {
        notify("Unrecognized key %s", argv[1]);
        return 0;
    }

    if (argv[2] == NULL) {
        keymap_del(&cfg->keymap, key);
        notify("Keymap removed successfully");
        return 0;
    }

    int ok = keymap_set(&cfg->keymap, key, argv[2], argv[3]);
    if (ok < 0) {
        notify("An error occured while trying to add mapping: %d", ok);
    }

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

static sidefx call_resize(const char** argv, void* cfg_) {
    struct tab_state* tab = cfg_;

    if (argv[1] == NULL) {
        notify("Must specify change amount");
        return 0;
    }

    uint8_t* col;
    if (tab->visual_sort & FIELD_NAME) {
        col = &tab->display.name_cols;
    } else if (tab->visual_sort & FIELD_GM) {
        col = &tab->display.gm_cols;
    } else if (tab->visual_sort & FIELD_LN) {
        col = &tab->display.ln_cols;
    } else {
        return 0;
    }

    int change = atoi(argv[1]);
    change = MAX(MIN(UINT8_MAX - *col, change), -(*col) + 1);

    *col += change;

    return REFRESH_LIST;
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
        .cmd = "masterlist",
        .call = call_masterlist,
        .expected_args = 2,
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
        .cmd = "clients",
        .call = call_clients,
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
        .cmd = "tabdel",
        .call = call_tabdel,
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
    {
        .cmd = "resize",
        .call = call_resize,
        .expected_args = 2,
    },
};

const struct regcmd* get_cmd(const char* str) {
    if (str == NULL) return NULL;
    for (size_t i = 0; i < sizeof(cmd_arr) / sizeof(cmd_arr[0]); i++) {
        if (strcmp(str, cmd_arr[i].cmd) == 0) {
            return cmd_arr + i;
        }
    }
    return NULL;
}

void* lvl_pointer(uint8_t lvl, struct app_state* cfg) {
    if (cfg == NULL) return NULL;

    void* arg;
    switch (lvl) {
        case LVL_TAB:
            arg = cfg->tabs + cfg->tabs_selected;
            break;
        case LVL_APPLICATION:
            arg = cfg;
            break;
        default:
            arg = NULL;
    }

    return arg;
}

sidefx call_cmd(const struct regcmd* cmd, const char** argv,
                struct app_state* cfg) {
    if (cmd == NULL || (cmd->expected_args != 0 && argv == NULL)) return 0;

    return cmd->call(argv, lvl_pointer(cmd->lvl, cfg));
}

sidefx handle_cmd(char* cmd, struct app_state* cfg) {
    if (cmd == NULL || cfg == NULL) return 0;

    char* toks[CMD_TOKS + 1] = {0};
    size_t parsed_toks = parse_toks(cmd, toks, 2);
    if (parsed_toks < 1) return 0;

    const struct regcmd* found = get_cmd(toks[0]);
    if (found == NULL) return 0;

    if (found->expected_args > 1) {
        parsed_toks += parse_toks(toks[1], toks + 1, found->expected_args - 1);
    }

    return call_cmd(found, (const char**)toks, cfg);
}
