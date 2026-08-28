// =============================================================================
//  names.h  -  Random name generator word banks + creature phrase lines.
//  Included once by the main sketch.
// =============================================================================
#pragma once
#include <Arduino.h>

// ---- Name generator ---------------------------------------------------------
// A random, per-device name is built at first boot from hardware entropy and
// stored in NVS, e.g. "Byte-Gremlin", "Neon-Packet".
static const char* NAME_ADJ[] = {
  "Neon","Byte","Ghost","Cyber","Static","Pixel","Turbo","Echo","Rogue","Quantum",
  "Fuzzy","Glitch","Solar","Volt","Nano","Hex","Crypto","Radio","Sonic","Phantom",
  "Lucky","Grumpy","Sneaky","Wired","Chrome","Frost","Ember","Zappy","Mega","Vapor"
};
static const char* NAME_NOUN[] = {
  "Gremlin","Packet","Sniffer","Byte","Blip","Router","Beacon","Daemon","Goblin","Sprite",
  "Wisp","Bot","Node","Chip","Gizmo","Critter","Nibble","Signal","Spark","Pixel",
  "Fox","Moth","Otter","Raccoon","Cat","Owl","Bat","Newt","Slug","Drone"
};
static const int NAME_ADJ_N  = sizeof(NAME_ADJ)  / sizeof(NAME_ADJ[0]);
static const int NAME_NOUN_N = sizeof(NAME_NOUN) / sizeof(NAME_NOUN[0]);

// ---- Phrase banks (kept short to fit a speech bubble) -----------------------
static const char* SAY_GREET[] = {
  "hi!", "beep boop", "sniff sniff", "any wifi?", "let's roam", "feed me pkts"
};
static const char* SAY_PET[] = {
  "hehe", "more!", "purr...", "^_^", "i like you", "boop back"
};
static const char* SAY_FED[] = {
  "nom nom", "tasty pkt", "yum data", "burp", "so full", "more aps pls"
};
static const char* SAY_PLAY[] = {
  "wheee!", "again!", "zoom zoom", "so fun", "dizzy...", "tag!"
};
static const char* SAY_LEVELUP[] = {
  "level up!", "i grew!", "stronger!", "new powers", "evolving~", "getting big"
};
static const char* SAY_DISCOVERY[] = {
  "ooh new!", "gotcha", "a signal!", "logged it", "who's this?", "+xp!"
};
static const char* SAY_HUNGRY[] = {
  "so hungry", "need pkts", "feed me...", "tummy rumble", "find aps!", "starving"
};
static const char* SAY_SLEEPY[] = {
  "yawn...", "so tired", "need rest", "zzz", "low energy", "let me nap"
};

#define PHRASE_PICK(arr) (arr[esp_random() % (sizeof(arr)/sizeof(arr[0]))])
