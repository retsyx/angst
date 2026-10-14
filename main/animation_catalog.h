#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* IDs follow animations.md and are also the web control API's stable values. */
typedef enum {
    ANIMATION_NONE = 0,
    ANIMATION_CLEAN = 1,
    ANIMATION_MINE,
    ANIMATION_PAN,
    ANIMATION_DRIVE,
    ANIMATION_PUNCH,
    ANIMATION_NARUTO,
    ANIMATION_CRANK,
    ANIMATION_SLOT_MACHINE,
    ANIMATION_SHADOW_BOX,
    ANIMATION_COINS,
    ANIMATION_BROOM,
    ANIMATION_DOG,
    ANIMATION_CAT,
    ANIMATION_SLEEP,
    ANIMATION_VIDEO_GAMES,
    ANIMATION_UPSET,
    ANIMATION_SMOKE,
    ANIMATION_COFFEE,
    ANIMATION_COOK,
    ANIMATION_SWORD_FIGHT,
    ANIMATION_ROMANCE,
    ANIMATION_FLY,
    ANIMATION_TELEPORT,
    ANIMATION_MUSIC,
    ANIMATION_CHILL,
    ANIMATION_DRAW,
    ANIMATION_GYMNASTICS,
    ANIMATION_PLANTS,
    ANIMATION_PHONE,
    ANIMATION_CELEBRATE,
    ANIMATION_PARTY,
    ANIMATION_NEWSPAPER,
    ANIMATION_TREASURE,
    ANIMATION_COUNT = ANIMATION_TREASURE
} animation_id_t;

typedef enum {
    ANIMATION_STATE_IDLE = 1,
    ANIMATION_STATE_UPDATE = 2,
    ANIMATION_STATE_UP = 4,
    ANIMATION_STATE_DOWN = 8
} animation_state_t;

typedef struct {
    animation_id_t id;
    const char *name;
    uint8_t allowed_states;
    uint32_t duration_ms;
} animation_info_t;

static const animation_info_t animation_catalog[ANIMATION_COUNT] = {
    {ANIMATION_CLEAN, "Clean the screen", ANIMATION_STATE_IDLE, 10000},
    {ANIMATION_MINE, "Mine for gold", ANIMATION_STATE_IDLE | ANIMATION_STATE_UP, 11000},
    {ANIMATION_PAN, "Pan for gold", ANIMATION_STATE_IDLE | ANIMATION_STATE_UP, 11000},
    {ANIMATION_DRIVE, "Pretend to drive", ANIMATION_STATE_IDLE, 10000},
    {ANIMATION_PUNCH, "Punch and kick the numbers", ANIMATION_STATE_UPDATE, 5000},
    {ANIMATION_NARUTO, "Naruto run", ANIMATION_STATE_IDLE, 9000},
    {ANIMATION_CRANK, "Crank the numbers", ANIMATION_STATE_UPDATE, 5000},
    {ANIMATION_SLOT_MACHINE, "Slot machine", ANIMATION_STATE_UPDATE, 5000},
    {ANIMATION_SHADOW_BOX, "Shadow boxing", ANIMATION_STATE_IDLE, 10000},
    {ANIMATION_COINS, "Golden coin shower", ANIMATION_STATE_UP, 7000},
    {ANIMATION_BROOM, "Dance with a broom", ANIMATION_STATE_IDLE, 10000},
    {ANIMATION_DOG, "Play with a dog", ANIMATION_STATE_IDLE, 12000},
    {ANIMATION_CAT, "Play with a cat", ANIMATION_STATE_IDLE, 12000},
    {ANIMATION_SLEEP, "Sleep in bed", ANIMATION_STATE_IDLE, 12000},
    {ANIMATION_VIDEO_GAMES, "Play video games", ANIMATION_STATE_IDLE, 11000},
    {ANIMATION_UPSET, "Upset in a lawn chair", ANIMATION_STATE_DOWN, 9000},
    {ANIMATION_SMOKE, "Smoke break", ANIMATION_STATE_IDLE, 10000},
    {ANIMATION_COFFEE, "Make and drink coffee", ANIMATION_STATE_IDLE, 12000},
    {ANIMATION_COOK, "Cook a meal", ANIMATION_STATE_IDLE, 12000},
    {ANIMATION_SWORD_FIGHT, "Sword fight a double", ANIMATION_STATE_IDLE, 12000},
    {ANIMATION_ROMANCE, "Meet a girl", ANIMATION_STATE_IDLE, 12000},
    {ANIMATION_FLY, "Fly around", ANIMATION_STATE_IDLE, 10000},
    {ANIMATION_TELEPORT, "Teleport", ANIMATION_STATE_IDLE, 10000},
    {ANIMATION_MUSIC, "Play music", ANIMATION_STATE_IDLE, 12000},
    {ANIMATION_CHILL, "Chill on the numbers", ANIMATION_STATE_IDLE, 11000},
    {ANIMATION_DRAW, "Draw on the screen", ANIMATION_STATE_IDLE, 11000},
    {ANIMATION_GYMNASTICS, "Perform gymnastics", ANIMATION_STATE_IDLE, 11000},
    {ANIMATION_PLANTS, "Grow and water plants", ANIMATION_STATE_IDLE, 12000},
    {ANIMATION_PHONE, "Check the phone", ANIMATION_STATE_IDLE, 10000},
    {ANIMATION_CELEBRATE, "Dance and celebrate", ANIMATION_STATE_UP, 9000},
    {ANIMATION_PARTY, "House party", ANIMATION_STATE_UP, 12000},
    {ANIMATION_NEWSPAPER, "Read a newspaper", ANIMATION_STATE_IDLE, 11000},
    {ANIMATION_TREASURE, "Dig for treasure", ANIMATION_STATE_UP, 12000}
};

static inline const animation_info_t *animation_catalog_lookup(animation_id_t id)
{
    return id >= ANIMATION_CLEAN && id <= ANIMATION_COUNT
               ? &animation_catalog[(size_t)id - 1] : NULL;
}

static inline bool animation_catalog_allowed(animation_id_t id, animation_state_t state)
{
    const animation_info_t *info = animation_catalog_lookup(id);
    unsigned states = (unsigned)state;
    if (states & (ANIMATION_STATE_UP | ANIMATION_STATE_DOWN)) states |= ANIMATION_STATE_UPDATE;
    return info && (info->allowed_states & states) != 0;
}
