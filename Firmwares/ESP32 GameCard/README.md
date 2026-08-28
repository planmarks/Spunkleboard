# Gamecard

A pocket game console for the **Seeed XIAO ESP32-C3** with a 0.96" OLED, six
buttons, and a LiPo battery. Single-player games plus multiplayer over
**ESP-NOW** — a host advertises a lobby and nearby units running this firmware
join it.

## Hardware

| Part            | Detail                                             |
|-----------------|----------------------------------------------------|
| MCU             | XIAO ESP32-C3                                       |
| Display         | 0.96" SSD1306 OLED, 128×64, I2C @ `0x3C`            |
| Buttons         | 6× tactile, wired **GPIO → button → GND**           |
| Power           | 3V LiPo on the XIAO's BAT pads                      |

### Pin map (XIAO ESP32-C3)

| Function  | Silk | GPIO |
|-----------|------|------|
| OLED SDA  | D4   | 6    |
| OLED SCL  | D5   | 7    |
| UP        | D0   | 2    |
| DOWN      | D1   | 3    |
| OK (A)    | D2   | 4    |
| BACK (B)  | D3   | 5    |
| LEFT      | D8   | 8    |
| RIGHT     | D9   | 9    |

Buttons use the internal pull-ups (`INPUT_PULLUP`), so each button just needs to
short its GPIO to GND — no external resistors.

> **Boot caution:** D0/D8/D9 (GPIO2/8/9) are ESP32-C3 *strapping* pins, and
> GPIO9 is the BOOT pin. Because buttons are active-low, holding **RIGHT** (and
> ideally UP/LEFT) while powering on or resetting can put the chip into download
> mode instead of running the sketch. Just don't hold those during reset.

> If your OLED shows nothing, try address `0x3D` in `common.h` (`OLED_ADDR`).

## Build / flash

- **Board:** *XIAO_ESP32C3* (install "esp32 by Espressif" boards, **v3.x**).
- **Libraries:** *Adafruit GFX* and *Adafruit SSD1306* (Library Manager).
- Open `gamecard.ino` in the Arduino IDE (it pulls in the other `.ino` files in
  this folder automatically), select the board/port, and Upload.

The ESP-NOW receive callback is written for Arduino-ESP32 **core 3.x** but falls
back to the 2.x signature automatically.

## Controls

- **Menus:** UP/DOWN move (the list scrolls), **A** select, **B** back.
- **B** quits any game back to the menu; on a game-over screen **A** replays.

## Games

Single-player:

| Game | Controls |
|------|----------|
| **Snake** | D-pad to steer |
| **Tetris** | LEFT/RIGHT move, **A** rotate, DOWN soft drop, UP hard drop |
| **2048** | D-pad slides tiles |
| **Runner** | **A**/UP to jump obstacles (endless) |
| **Breakout** | LEFT/RIGHT move paddle |
| **Invaders** | LEFT/RIGHT move, **A** shoot |
| **Minesweeper** | D-pad move cursor, tap **A** reveal, **hold A** to flag |
| **Simon** | Watch the flashing pads, repeat with the D-pad |

Multiplayer:

- **Pong** — `1 Player` (vs CPU), `Host Game`, or `Join Game` (2 players).
- **Tron** — light-cycles for **2-4 players**. `Host Game` opens a lobby; other
  units `Join Game`. The host presses **A** to start once at least one player has
  joined. Steer with the D-pad (no reversing); the ring marks your own cycle.
  Last one riding wins.
- **Battleship** — **2 players**. Each unit hides its own 8x8 fleet (the two
  private screens are the whole point). Placement: D-pad reshuffles a random
  layout, **A** = ready. Battle: D-pad moves the radar cursor, **A** fires. Sink
  the enemy fleet first.
- **Uno** — **2-4 players**. Standard deck and rules (skip, reverse, +2, wild,
  wild +4). On your turn, LEFT/RIGHT pick a card or the **DRAW** slot, **A** to
  confirm; a wild then asks for a colour via the D-pad. First to empty their hand
  wins.

## Multiplayer (ESP-NOW)

1. On one unit: **Pong → Host Game**. It advertises a lobby using its device
   name.
2. On another unit: **Pong → Join Game**. It lists nearby lobbies; pick one and
   press **A**.
3. The host accepts and both drop into a match. The **host** simulates the ball
   and left paddle; the **client** controls the right paddle and renders the
   host's authoritative state.

All units stay on the default WiFi channel (they never join an access point), so
no channel setup is needed. Traffic is tagged with a magic byte so foreign
ESP-NOW packets are ignored.

## Device name

Each unit has a unique name (defaults to `GC-XXXX` from its MAC). Change it in
**Settings**: LEFT/RIGHT move the cursor, UP/DOWN change the character, **A**
saves (persisted in NVS), **B** cancels. The name is what other units see when
browsing lobbies (up to 10 characters).

## Files

| File           | Role                                             |
|----------------|--------------------------------------------------|
| `gamecard.ino` | globals, `setup`/`loop`, scrolling menu + registry|
| `common.h`     | shared declarations                              |
| `input.ino`    | button debounce / edge detection / auto-repeat   |
| `settings.ino` | device name editor + NVS persistence             |
| `netplay.ino`  | ESP-NOW lobby + transport                        |
| `snake.ino`    | Snake                                            |
| `tetris.ino`   | Tetris                                           |
| `g2048.ino`    | 2048                                             |
| `runner.ino`   | endless jumper                                   |
| `breakout.ino` | Breakout                                         |
| `invaders.ino` | Space Invaders                                   |
| `mines.ino`    | Minesweeper                                      |
| `simon.ino`    | Simon                                            |
| `pong.ino`     | Pong (single + multiplayer)                      |
| `netmp.ino`    | multiplayer transport (up to 4 players)          |
| `mplobby.ino`  | generic host/join lobby for turn-based games     |
| `tron.ino`     | Tron light-cycles (2-4 players)                  |
| `bship.ino`    | Battleship (2 players)                           |
| `uno.ino`      | Uno (2-4 players)                                |

## Adding a single-player game

1. Write `myGameInit()` / `myGameUpdate()` in a new `.ino` and declare them in
   `common.h`. The update fn returns to the menu with `setState(ST_MENU)`.
2. Add a launch shim + registry row in `gamecard.ino`:
   `static void L_mine(){ startGame(myGameInit, myGameUpdate); }` and a
   `{ "My Game", L_mine }` entry in `GAMES[]`.

Because all `.ino` files compile into one translation unit, give every
file-scope `static` a game-specific name (e.g. `tScore`, not `score`) to avoid
collisions.

## Multiplayer architecture

Two transport layers share the ESP-NOW radio and the `NET_ADV` lobby discovery:

- **Pong** (`netplay.ino`) — the original 1-host/1-client path.
- **Up to 4 players** (`netmp.ino`) — 1 host + up to 3 clients. Two styles share
  it:
  - *Real-time* (Tron): host is authoritative and **broadcasts** compact state
    each tick; clients unicast input back.
  - *Turn-based* (Battleship, Uno): a generic `NET_MMSG` carries per-game
    messages (`p[]` + a 40-byte `d[]` for things like Uno hands), addressed
    host↔client, delivered through a small receive queue (`mpPoll`). Battleship
    is peer-to-peer shot/reply; Uno is host-as-dealer with private hands.

  Messages carry a player-slot id, and receivers filter by game id + the peer's
  MAC so two nearby games don't cross wires. Turn-based exchanges resend on a
  timer (and Uno re-broadcasts state every 700 ms) so one lost packet can't
  deadlock a turn.

## Roadmap

More games on the `netmp.ino` layer: a trivia/reaction buzzer and Connect 4 are
natural next additions.
