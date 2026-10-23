# sampservb

A unix-native TUI server browser/launcher for the San Andreas Multiplayer client modification, written in POSIX(\*) C99 with portability in mind.

![Screenshot](https://raw.githubusercontent.com/anny-moose/sampservb/refs/heads/main/screenshot.png)

SA-MP is a client modification and a server for Grand Theft Auto: San Andreas (2004) which adds multiplayer capabilities to GTA:SA, allowing for various custom gamemodes including, but not limited to: PVP, Roleplay, Racing, Stunt driving. The original project is abandoned and lives on thanks to the open source community reimplementation, [open.mp](https://open.mp/). The open.mp launcher is written in Tauri and currently isn't officially supported on Linux, resulting in [certain difficulties](https://github.com/openmultiplayer/launcher/issues/10) stemming from the fact that it's a web application. 

### # Features
- Vim-like `:commands`
- Fetching a list of servers via an api
- Fetching a list of servers via a masterlist
- Tab-specific username/game exe

## # Building
Building this project should be relatively easy, as the only things you need are `ncurses`, `cJSON`, `libcurl`, `gmake` and working `pkg-config`.

Here's an example of how to install dependencies on Fedora Linux.
```
# dnf install ncurses-devel libcurl-devel cjson-devel make gcc
```

After installing dependencies, you can proceed to building by cloning the repo
```
$ git clone https://github.com/anny-moose/sampservb
```

```
$ cd sampservb
$ gmake
```

After building, you can run it by executing `build/out`

The default build target is `DEBUG`, which builds the code with ASan, UBSan and disables optimizations. 
You may build the release target by invoking
```
$ TARGET=RELEASE gmake
```

Which will build the project in the `release` folder.

## # Usage
After starting the launcher you will be greeted with an empty serverlist, which you can populate by either running `:masterlist` or `:fetch`

The default controls are described in the following table

| Key     | Command                | Action                                |
|---------|------------------------|---------------------------------------|
| `j`     | `:next`                | Scroll to the next server             |
| `k`     | `:prev`                | Scroll to the previous server         |
| `h`     | `:sortprev`            | Scroll to the previous column   |
| `l`     | `:sortnext`            | Scroll to the next column       |
| `<Tab>` | `:sort`                | Sort the list                         |
| `c`     | `:clients`             | Query the list of connected players   |
| `r`     | `:rules`               | Query the server rules                |
| `R`     | `:refetch`             | Update server info                    |
| `e`     | `:set filter empty`    | Filter out servers without players    |
| `f`     | `:set filter full`     | Filter out servers without free slots |
| `p`     | `:set filter password` | Filter out password protected servers |
| `<CR>`  | `:connect`             | Connect to the selected server        |
| `1`-`3` | `:tab 1` - `:tab 3`    | Switch to the tab.                    |
| `=` | `:resize 5`    | Expand the currently selected column by 5                    |
| `-` | `:resize -5`    | Shrink the currently selected column by 5                    |

In order to connect to a server, you have to set your username and the game executable first, which you can do by invoking `:set username <name>` and `:set exec_cmd <path>` respectively.

On the topic of setting the game executable, you'll probably have to create one beforehand.

In order to avoid the responsibility of managing a wine prefix or to prevent locking the user into one rigid way of launching the game, the burden of launching the game partially falls into the user's hands. You, as the user, have to provide the launcher with an executable that can be launched with the arguments `--name`, `--host`, `--port`, and optionally, `--password` (which is only required for connecting to password-protected servers)

The easiest way to get an executable like that, is to follow [this guide](https://open.mp/docs/client/ClientOnLinux) to get the open.mp client executable, optionally skipping the omp-client.dll step, as it is not mandatory for this to work, and then create a shell script like the following

```sh
#!/bin/sh

<your wine command> <path to omp-launcher.exe> "$@" --gamepath '<in-prefix-accessible path to the game directory>' 
```

and then make it executable with `chmod +x <path to script>`

### # Fetching servers from a different API
Both `:masterlist` and `:fetch` support custom endpoints, while the default endpoints for them are https://sam.markski.ar/api/GetMasterlist and https://api.open.mp/servers respectively, you can call them with an optional argument for the address from which the list of IPs or the JSON serverlist will be fetched from. In the case of `:fetch`, you may also add a second argument describing the names of the json keys, e.g.

```
:fetch http://sam.markski.ar/api/GetAllServers requiresPassword;ipAddr;playersOnline;maxPlayers;name;gameMode;language
```

### # Tabs
You can create a tab with `:tabnew` and switch tabs with the keybinds or `:tab n` where `n` denotes the number of the tab. Every tab has its own server list, which you can populate in any way you desire. Another feature of tabs is the fact that each tab has its own configuration for e.g the executable, meaning that you can conveniently separate your servers into different tabs depending on things like the version of the client that's required, or the username that you use.

You can give your tabs a memorable name by invoking `:set tabname <name>`

### # Manually adding a server to a server list
You can manage a favourite server list just like you could in the regular samp client, by invoking `:add` with the server's IP address or hostname, optionally followed by a port as the __second argument__. (assumed to be `7777` if not present)\
Example:
```
:add 127.0.0.1 7777
:add 127.0.0.1
```

### # Managing a configuration file
Another part that was inspired by Vim was the fact that state isn't persistent across launches, meaning that if you restart the launcher, you will find that you will once again be greeted by an empty server list. To counteract that, you can create an `init.rc` file in the `sampservb` folder in your XDG configuration folder. (typically `~/.config`)

Below is an example configuration you could build off of.
```
set username Jane_Doe
set tabname Favourites

tabnew
set username gamer11
masterlist
set sort players
set filter empty
set filter full
set filter password
set tabname Masterlist
```

## # Project status
The project is still in development, however it has reached a state where it's usable. There are multiple planned features, including character detection and conversion for servers that don't use a UTF-8 encoding, rewriting the command prompt to not block, and more.

## # Portability
Aside from libraries such as `ncurses`, `libcurl` and `cJSON`, BSD `err.h` is *currently* used in the source code, making it not fully standard-compliant.



