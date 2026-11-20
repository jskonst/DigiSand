#include "pitches.h"

// Метроном для занятий. Темп 40..208.
// Размеры по кругу: выкл / 2/4 / 3/4 / 4/4 / 3/8 / 6/8 / 8/4.
// Щелчок ставится по micros() и не ждёт перерисовку матрицы или физику песка.

#define METRO_MIN 40
#define METRO_MAX 208

// Индекс в EEPROM не сдвигается: новый размер только в конец.
// 0 выкл, 1 = 2/4, 2 = 3/4, 3 = 4/4, 4 = 3/8, 5 = 6/8, 6 = 8/4
static const uint8_t METER_BEATS[] PROGMEM = {0, 2, 3, 4, 3, 6, 8};
static const uint8_t METER_CODE[] PROGMEM = {0, 2, 3, 4, 38, 68, 84};
static const uint8_t METER_COUNT = sizeof(METER_BEATS);
static_assert(sizeof(METER_BEATS) == sizeof(METER_CODE), "meter tables differ");

static bool metroRunning = 0;
static bool hitLeft = 0;
static bool metroDirty = 0;
static uint8_t metroBeat = 0;   // 0..n-1, доля, которая прозвучит следующей
static uint8_t shownBeat = 0;   // 1..n, доля, которая звучит сейчас
static uint8_t flashKind = 0;   // 0 обычная, 1 слабая (середина 6/8 и 8/4), 2 сильная
static uint16_t chirpFreq = 0;
static uint16_t chirpDur = 0;
static uint16_t intervalErr = 0;  // 60000000 % bpm, микросекунды, которые надо «размазать»
static uint16_t errAcc = 0;
static uint32_t intervalUs = 500000;
static uint32_t nextUs = 0;
static uint32_t beatStartUs = 0;
static uint32_t chirpUs = 0;
static uint32_t flashUntil = 0;
static uint32_t metroDrawMs = 0;

uint8_t metroMeterCount() {
  return METER_COUNT;
}

uint8_t metroBeats() {
  uint8_t m = data.meter;
  if (m >= METER_COUNT) return 0;
  return pgm_read_byte(&METER_BEATS[m]);
}

uint8_t metroMeterCode() {
  uint8_t m = data.meter;
  if (m >= METER_COUNT) m = 0;
  return pgm_read_byte(&METER_CODE[m]);
}

// интервал без накопления ошибки: за bpm долей проходит ровно 60 000 000 мкс
static void metroApplyBpm() {
  if (data.bpm < METRO_MIN || data.bpm > METRO_MAX) data.bpm = 80;
  intervalUs = 60000000UL / data.bpm;
  intervalErr = (uint16_t)(60000000UL % data.bpm);
  errAcc = 0;
}

static void playTick(uint8_t kind) {
  chirpUs = 0;
  if (data.vol <= 0) return;
  if (kind == 2) {
    // сильная доля: громкий щелчок и следом более высокий — ритм, а не только высота
    tone(SOUND_PIN, NOTE_C7, 14);
    chirpFreq = NOTE_G7;
    chirpDur = 18;
    chirpUs = micros() + 24000UL;
  } else if (kind == 1) {
    tone(SOUND_PIN, NOTE_G7, 26);
  } else {
    tone(SOUND_PIN, NOTE_C7, 22);
  }
}

static void metroFire(uint32_t scheduled, uint32_t now) {
  uint8_t n = metroBeats();
  uint8_t kind = 0;
  if (n && metroBeat == 0) kind = 2;
  // Середина чётного такта от 6 долей: 6/8 — 4-я, 8/4 — 5-я. 2/4 и 4/4 без второго акцента.
  else if (n >= 6 && metroBeat * 2 == n) kind = 1;

  playTick(kind);
  // маятник привязан к сетке доли, а не к дрожанию цикла; после большого опоздания — заново
  if ((int32_t)(now - scheduled) > 20000) scheduled = now;
  hitLeft = !hitLeft;
  beatStartUs = scheduled;
  flashKind = kind;
  flashUntil = millis() + (kind == 2 ? 70 : 45);

  if (n) {
    shownBeat = metroBeat + 1;
    metroBeat++;
    if (metroBeat >= n) metroBeat = 0;
  } else {
    shownBeat = 0;
  }
  metroDirty = 1;
}

// true, если только что прозвучала доля
bool metroPoll() {
  uint32_t now = micros();
  if (metroRunning && (int32_t)(now - nextUs) >= 0) {
    chirpUs = 0;
    metroFire(nextUs, now);
    nextUs += intervalUs;
    errAcc += intervalErr;
    if (errAcc >= data.bpm) {
      errAcc -= data.bpm;
      nextUs++;
    }
    // если цикл опоздал больше чем на долю — не выдавать пачку щелчков
    if ((int32_t)(now - nextUs) >= 0) {
      nextUs = now + intervalUs;
      errAcc = 0;
    }
    return true;
  }
  if (chirpUs && (int32_t)(now - chirpUs) >= 0) {
    chirpUs = 0;
    if (data.vol > 0) tone(SOUND_PIN, chirpFreq, chirpDur);
  }
  return false;
}

static uint8_t pendulumX() {
  if (!metroRunning || !intervalUs) return 3;
  uint32_t e = micros() - beatStartUs;
  if (e > intervalUs) e = intervalUs;
  uint8_t pos = (uint8_t)((e * 7UL) / intervalUs);
  return hitLeft ? pos : (uint8_t)(7 - pos);
}

static uint8_t beatDotX(uint8_t i, uint8_t n) {
  if (n == 2) return i ? 6 : 1;
  if (n == 3) return i == 0 ? 1 : (i == 1 ? 4 : 6);
  if (n == 6) return i < 3 ? i : (uint8_t)(i + 2);  // две группы по три: 6/8
  if (n == 8) return i;                             // 8/4: вся верхняя строка, x = 0..7
  return (uint8_t)(i * 2);                          // 4/4: 0,2,4,6
}

// три цифры на матрицу 8 пикселей: сотни — узкий столбец, остальное шрифт 3x5
static void printBpm(uint8_t bpm) {
  if (bpm < 100) {
    printDig(&mtrx, 8, 1, bpm / 10);
    printDig(&mtrx, 12, 1, bpm % 10);
    return;
  }
  if (bpm < 200) {
    for (uint8_t y = 1; y <= 5; y++) mtrx.dot(8, y);  // «1», как в шрифте
  } else {
    // «2» шириной в 2 столбца, только темпы 200..208
    mtrx.dot(8, 1);
    mtrx.dot(9, 1);
    mtrx.dot(9, 2);
    mtrx.dot(8, 3);
    mtrx.dot(8, 4);
    mtrx.dot(8, 5);
    mtrx.dot(9, 5);
  }
  printDig(&mtrx, 10, 1, (bpm / 10) % 10);
  printDig(&mtrx, 13, 1, bpm % 10);
}

static void drawMetro() {
  mtrx.clear();
  printBpm(data.bpm);

  uint8_t n = metroBeats();
  if (!metroRunning) {
    // пауза
    for (uint8_t y = 2; y <= 4; y++) {
      mtrx.dot(2, y);
      mtrx.dot(5, y);
    }
  } else if (n && shownBeat) {
    printDig(&mtrx, 2, 1, shownBeat);
  }

  if (n) {
    for (uint8_t i = 0; i < n; i++) {
      if (!metroRunning || i < shownBeat) mtrx.dot(beatDotX(i, n), 0);
    }
  }

  uint8_t px = pendulumX();
  mtrx.dot(px, 6);
  mtrx.dot(px, 7);
  if (px > 0) mtrx.dot(px - 1, 7);
  if (px < 7) mtrx.dot(px + 1, 7);

  bool flashing = metroRunning && (int32_t)(millis() - flashUntil) < 0;
  if (flashing) {
    uint8_t edge = hitLeft ? 0 : 7;
    uint8_t y0 = flashKind == 2 ? 0 : (flashKind == 1 ? 2 : 4);
    mtrx.lineV(edge, y0, 7);
    if (flashKind == 2) mtrx.lineH(7, 8, 15);
    else {
      mtrx.dot(11, 7);
      mtrx.dot(12, 7);
    }
  }
  mtrx.update();
}

void changeMeter(int8_t dir) {
  int8_t m = (int8_t)data.meter + dir;
  if (m >= (int8_t)METER_COUNT) m = 0;
  if (m < 0) m = (int8_t)METER_COUNT - 1;
  data.meter = (uint8_t)m;
  metroBeat = 0;
  shownBeat = 0;
  metroDirty = 1;
  memory.update();
}

void changeBpm(int8_t dir) {
  int16_t v = (int16_t)data.bpm + dir;
  if (v < METRO_MIN) v = METRO_MIN;
  if (v > METRO_MAX) v = METRO_MAX;
  if ((uint8_t)v == data.bpm) return;
  data.bpm = (uint8_t)v;
  metroApplyBpm();
  if (metroRunning) {
    uint32_t now = micros();
    // ускорение не должно ждать старый длинный интервал
    if ((int32_t)(nextUs - now) > (int32_t)intervalUs) nextUs = now + intervalUs;
  }
  memory.update();
  metroDirty = 1;
}

static void metroStart() {
#ifdef SOUND_PIN
  StopMelody();
#endif
  metroRunning = 1;
  metroBeat = 0;
  hitLeft = 0;
  shownBeat = 0;
  metroApplyBpm();
  nextUs = micros();
  metroPoll();  // сильная доля сразу, без ожидания кадра
  metroDirty = 1;
}

static void metroStop() {
  metroRunning = 0;
  chirpUs = 0;
  noTone(SOUND_PIN);
  flashUntil = 0;
  shownBeat = 0;
  metroDirty = 1;
}

void metroHalt() {
  if (metroRunning) metroStop();
  else {
    chirpUs = 0;
    noTone(SOUND_PIN);
  }
}

void drawMetroField() {
  // 01 на поле вкл/выкл — тот же маятник, что на темпе. Отдельный шаг к полю темпа не нужен.
  if (editField == 1 || (editField == 0 && metroRunning)) {
    drawMetro();
    return;
  }
  mtrx.clear();
  printIcon(&mtrx, 0, 0, 7);
  if (editField == 0) {
    printDig(&mtrx, 8, 1, 0);
    printDig(&mtrx, 12, 1, data.metro ? 1 : 0);
  } else {
    uint8_t code = metroMeterCode();
    printDig(&mtrx, 8, 1, code / 10);
    printDig(&mtrx, 12, 1, code % 10);
    // размер при работающем метрономе: две точки, что щелчки и маятник уже идут
    if (metroRunning && ((millis() / 400) & 1)) {
      mtrx.dot(0, 0);
      mtrx.dot(1, 0);
    }
  }
  mtrx.update();
}

void enterMetroEdit() {
  beginEditor(2, MENU_METRO);
  if (data.metro) metroStart();
  else metroHalt();
  metroDirty = 0;
  drawMetroField();
}

// dir = ±1 с клика или ±10 с удержания. Размер и вкл/выкл шагают на 1.
void nudgeMetroEditor(int8_t dir) {
  int8_t step = dir > 0 ? 1 : -1;
  if (editField == 0) {
    int8_t v = (int8_t)data.metro + step;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    data.metro = (uint8_t)v;
    memory.update();
    if (data.metro) {
      if (!metroRunning) metroStart();
    } else if (metroRunning) {
      metroStop();
    }
  } else if (editField == 1) {
    changeBpm(dir);
  } else {
    changeMeter(step);
  }
  editorDirty = 1;
}

void serviceMetroEditor(bool beat) {
  // поле темпа и только что включённый метроном: маятник, доля и BPM
  if (editField == 1 || (editField == 0 && metroRunning)) {
    uint32_t ms = millis();
    bool anim = metroRunning && (uint32_t)(ms - metroDrawMs) >= 25;
    if (!(beat || metroDirty || editorDirty || anim)) return;
    metroDrawMs = ms;
    metroDirty = 0;
    editorDirty = 0;
    drawMetro();
    // доля могла наступить за время SPI — щелчок сразу, картинка следом
    if (metroPoll()) drawMetro();
    return;
  }
  if (editField == 2 && metroRunning) {
    static uint8_t hintPhase = 0xff;
    uint8_t phase = (millis() / 400) & 1;
    if (!editorDirty && !metroDirty && phase == hintPhase) return;
    hintPhase = phase;
    editorDirty = 0;
    metroDirty = 0;
    drawMetroField();
    return;
  }
  if (!editorDirty) return;
  editorDirty = 0;
  metroDirty = 0;
  drawMetroField();
}
