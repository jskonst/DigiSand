#include <Arduino.h>
#include <stddef.h>

#include "print3x5.h"

#define PART_AMOUNT 58  // общее число песчинок на поле
#define BTN1_PIN 2
#define BTN2_PIN 3
#define CS_PIN 6
#define DT_PIN 4
#define CK_PIN 5
#define SOUND_PIN 9

#define MaxVolume 1  // максимальный уровень громкости эффектов
#define MaxMelody 5  // число имеющихся мелодий (для корректной работы меню)

#define battery_min 3000  // минимальный уровень заряда батареи для отображения 3000
#define battery_max 3800  // максимальный уровень заряда батареи для отображения  3800

// Структура конфигурации для хранения в EEPROM.
// Хвост только дописывается: ключ 'a' тот же, яркость и мелодии со старых прошивок не сдвигаются.
struct Data {
  int16_t sec = 60;  // время стороны A, секунды. Если dual == 0 — единственное время
  int8_t bri = 2;    // яркость
  int8_t vol = 1;    // громкость эффектов
  int8_t mel = 1;    // мелодия окончания времени
  int8_t ani = 0;    // анимации окончания времени
  int8_t tmpo = 4;   // темп проигрывания мелодий
  int8_t bat = 1;
  uint8_t bpm = 80;   // метроном, 40..208
  uint8_t meter = 3;  // 0 без акцента, 1 = 2/4, 2 = 3/4, 3 = 4/4, 4 = 3/8, 5 = 6/8
  int16_t secB = 60;  // время стороны B, секунды
  uint8_t dual = 0;   // 0 одно время на обе стороны, 1 у каждой своё
  uint8_t metro = 0;  // 0 метроном выключен, 1 включен
};
Data data;

// v1.3 — 8 байт, v1.4 дописала bpm и meter, v1.5 — secB, dual, metro.
// Стёртый EEPROM в хвосте равен 0xFF; это ловим при старте и подставляем значения по умолчанию.
static_assert(offsetof(Data, bpm) == 8, "bpm must stay appended after v1.3 settings");
static_assert(offsetof(Data, meter) == 9, "meter must follow bpm");
static_assert(offsetof(Data, secB) == 10, "secB appended after meter");
static_assert(offsetof(Data, dual) == 12, "dual follows secB");
static_assert(offsetof(Data, metro) == 13, "metro follows dual");
static_assert(sizeof(Data) == 14, "no padding in settings block");

#include <EEManager.h>
EEManager memory(data);

// button
#include <EncButton.h>
Button up(BTN1_PIN);
Button down(BTN2_PIN);
VirtButton dbl;

// matrix
#include <GyverMAX7219.h>
MAX7219<2, 1, CS_PIN, DT_PIN, CK_PIN> mtrx;

// mpu
#include "mini6050.h"
Mini6050 mpu;

// sandbox
#define BOX_W 16
#define BOX_H 16

#include "sand.h"
Sand<BOX_W, BOX_H> box;

// timer
#include "timer.h"
Timer fall_tmr, disp_tmr, menu_tmr;

// ============== ВАШ КОД ==============
// функция вызывается при каждом "проталкивании" песчинки
void onSandPush() {
#ifdef SOUND_PIN
  StopMelody();  // чтобы мелодия не накладывалась на звук падающего песка
  if (data.vol > 0) PlaySandPushTone();
#endif
}

// todo
//    сложность в том, что событие onSandEnd должно срабатывать именно тогда, когда весь песок пересыпался
//    с учётом возможных пауз из-за наклонов
//    возможно, тут получится сделать таймер, который будет отсчитывать настроенные секунды, делать паузу когда нужно и автоматом обнуляться, когда весь песок пересыпется
//    либо нужно отследить число частиц на втором экране
// функция вызывается при завершении времени отведённого времени
// void onSandEnd() {

// }

// функция вызывается, когда песок перестал сыпаться
// это происходит в том числе из-за крена часов, когда ещё не весь песок пересыпался
void onSandStop() {
#ifdef SOUND_PIN
  if (data.mel > 0) PlayMelody(data.mel);
#endif
}

// функция вызывается, когда заряд батареи ниже порога
void onBatteryEmpty() {
  mtrx.setBright(0);
  while (true) {
    mtrx.clear();
    printIcon(&mtrx, 0, 0, 6);
    mtrx.update();
    delay(1000);
    mtrx.clear();
    printIcon(&mtrx, 0, 0, 4);
    mtrx.update();
    delay(1000);
  }
}

#define MENU_COUNT 7
#define MENU_DUAL 6
#define MENU_METRO 7

uint8_t inMenu = 0;
uint8_t lastUsedMenu = 0;    // последнее использованное меню
int8_t fallDir = 1;          // >0 сверху сторона A (песок уходит из колбы 7,7), иначе сторона B
uint8_t editKind = 0;        // 0 нет, 1 экран двух времён, 2 экран метронома
uint8_t editField = 0;
uint8_t editMenu = 1;
bool editorDirty = 0;
unsigned long voltage;       // напряжение аккумулятора
float my_vcc_const = 1.080;  // константа вольтметра

// =====================================

bool runFlag = 1;
bool checkBound(int8_t x, int8_t y) {
  if (y >= 8 && x < 8) return 0;
  if (y < 8 && x >= 8) return 0;
  if (mpu.getDir() > 0) {
    if (x == 8 && y == 8) return 0;
  } else {
    if (x == 7 && y == 7) return 0;
  }
  return (x >= 0 && y >= 0 && x < BOX_W && y < BOX_H);
}
void setXY(int8_t x, int8_t y, bool value) {
  if (y >= 8) y -= 8;
  mtrx.dot(x, y, value);
}

void resetSand() {
  box.buf.clear();
  mtrx.clear();
  mtrx.update();
  for (uint8_t n = 0; n < PART_AMOUNT; n++) {
    box.buf.set(n % 8, n / 8, 1);
  }
}

void setup() {
  //Serial.begin(115200);
  Wire.begin();
  mpu.begin();
  memory.begin(0, 'a');
  // старая прошивка не писала хвост блока: в EEPROM там 0xFF, подставляем дефолт
  bool fixSettings = 0;
  if (data.bpm < 40 || data.bpm > 208) {
    data.bpm = 80;
    fixSettings = 1;
  }
  if (data.meter > 5) {
    data.meter = 3;
    fixSettings = 1;
  }
  if (data.secB < 0) {  // 0xFFFF со стёртого хвоста; 0 секунд — допустимое время
    data.secB = data.sec < 0 ? 60 : data.sec;
    fixSettings = 1;
  }
  if (data.dual > 1) {
    data.dual = 0;
    fixSettings = 1;
  }
  if (data.metro > 1) {
    data.metro = 0;
    fixSettings = 1;
  }
  if (fixSettings) memory.update();
  mtrx.begin();
  mtrx.setBright(data.bri);

  mpu.setX({ 1, -1 });
  mpu.setY({ 2, 1 });
  mpu.setZ({ 0, 1 });

  box.attachBound(checkBound);
  box.attachSet(setXY);

#ifdef SOUND_PIN
  pinMode(SOUND_PIN, OUTPUT);
  SetTempo(data.tmpo);
#endif

  voltage = readVcc();  // считать напряжение питания
  // Serial.println(voltage);
  if (voltage <= battery_min) onBatteryEmpty();
  showBattLevel(voltage);
  delay(2000);  // время отображения заряда после включения

  resetSand();
  mpu.update(0);
  fallDir = mpu.getDir();
  applyFallInterval();
}

// Верхняя сторона — та, из которой сейчас сыплется песок (см. fall()).
int16_t* upperTime() {
  if (data.dual && fallDir <= 0) return &data.secB;
  return &data.sec;
}

void applyFallInterval() {
  int16_t sec = *upperTime();
  if (sec < 0) sec = 0;
  fall_tmr.setInterval((uint32_t)sec * 1000ul / PART_AMOUNT);
}

void trackSide() {
  int8_t dir = mpu.getDir();
  if (dir == fallDir) return;
  fallDir = dir;
  if (data.dual) applyFallInterval();
}

// side < 0 — обычные часы без буквы, цифры на прежнем месте.
// side 0/1 — время A/B, буква под минутами и под секундами.
void showClock(int16_t sec, int8_t side) {
  if (sec < 0) sec = 0;
  uint8_t min = (uint8_t)(sec / 60);
  uint8_t s = (uint8_t)(sec % 60);
  int8_t y = side < 0 ? 1 : 0;
  mtrx.clear();
  printDig(&mtrx, 0, y, min / 10);
  printDig(&mtrx, 4, y, min % 10);
  printDig(&mtrx, 8, y, s / 10);
  printDig(&mtrx, 12, y, s % 10);
  if (side >= 0) {
    printSide(&mtrx, 2, 5, (uint8_t)side);
    printSide(&mtrx, 10, 5, (uint8_t)side);
  }
  mtrx.update();
}

void changeTime(int8_t dir) {
  disp_tmr.setTimeout(3000);
  if (data.dual && mpu.update(0)) fallDir = mpu.getDir();
  int16_t* sec = upperTime();
  int32_t v = (int32_t)(*sec) + dir;
  if (v < 0) v = 0;
  if (v > 32767) v = 32767;
  *sec = (int16_t)v;
  showClock(*sec, data.dual ? (fallDir <= 0 ? 1 : 0) : -1);
  applyFallInterval();
  memory.update();
}

// изменение темпа
void changeTempo(int8_t dir) {
  data.tmpo += dir;
  data.tmpo = constrain(data.tmpo, 1, 7);
  SetTempo(data.tmpo);
  PlayMelody(data.mel);
  memory.update();
}

// изменение анимации
void changeAni(int8_t dir) {
  data.ani += dir;
  data.ani = constrain(data.ani, 0, 1);
  memory.update();
}

// изменение яркости
void changeBri(int8_t dir) {
  data.bri += dir;
  data.bri = constrain(data.bri, 0, 15);
  mtrx.setBright(data.bri);
  memory.update();
}

// изменение громкости
void changeVol(int8_t dir) {
  data.vol += dir;
  data.vol = constrain(data.vol, 0, MaxVolume);
  memory.update();
}

// изменение мелодии
void changeMel(int8_t dir) {
  data.mel += dir;
  data.mel = constrain(data.mel, 0, MaxMelody);
  if (data.mel) PlayMelody(data.mel);
  else StopMelody();
  memory.update();
}

// изменение стиля отображения заряда
void changeBat(int8_t dir) {
  data.bat += dir;
  data.bat = constrain(data.bat, 0, 2);
  memory.update();
}

void enterMenu(int8_t menu) {
  disp_tmr.setTimeout(returnFromMenu, 5000);
  mtrx.clear();

  // здесь настраивается число доступных пунктов меню
  if (menu > MENU_COUNT) menu = 1;
  if (menu < 1) menu = MENU_COUNT;

  inMenu = menu;
  menu_tmr.setTimeout(forgetLastMenu, 30000);

  showMenu(menu);
}

void forgetLastMenu() {
  lastUsedMenu = 0;
}

void showMenu(int8_t menu) {
  // выводим иконку меню на первый экран
  // и текущее значение параметра на втворой
  switch (menu) {
    case 1:  // яркость
      printIcon(&mtrx, 0, 0, 2);
      printDig(&mtrx, 8 + 0, 1, data.bri / 10);
      printDig(&mtrx, 8 + 4, 1, data.bri % 10);
      break;

    case 2:  // громкость звуков
      printIcon(&mtrx, 0, 0, 0);
      printDig(&mtrx, 8 + 0, 1, data.vol / 10);
      printDig(&mtrx, 8 + 4, 1, data.vol % 10);
      break;

    case 3:  // мелодия окончания
      printIcon(&mtrx, 0, 0, 1);
      printDig(&mtrx, 8 + 0, 1, data.mel / 10);
      printDig(&mtrx, 8 + 4, 1, data.mel % 10);
      break;

    case 4:  // темп мелодий
      printIcon(&mtrx, 0, 0, 5);
      printDig(&mtrx, 8 + 0, 1, data.tmpo / 10);
      printDig(&mtrx, 8 + 4, 1, data.tmpo % 10);
      break;

    case 5:  // стиль отображения заряда при включении
      printIcon(&mtrx, 0, 0, 6);
      printDig(&mtrx, 8 + 0, 1, data.bat / 10);
      printDig(&mtrx, 8 + 4, 1, data.bat % 10);
      break;

    case MENU_DUAL:  // два времени, снизу вкл/выкл
      printIcon(&mtrx, 0, 0, 8);
      printDig(&mtrx, 8 + 0, 1, 0);
      printDig(&mtrx, 8 + 4, 1, data.dual ? 1 : 0);
      break;

    case MENU_METRO:  // метроном, снизу вкл/выкл. Включён — мигают две точки слева сверху.
      printIcon(&mtrx, 0, 0, 7);
      printDig(&mtrx, 8 + 0, 1, 0);
      printDig(&mtrx, 8 + 4, 1, data.metro ? 1 : 0);
      if (data.metro && ((millis() / 400) & 1)) {
        mtrx.dot(0, 0);
        mtrx.dot(1, 0);
      }
      break;
  }
  mtrx.update();
}

// Отображение уровня заряда в процентах или вольтах
void showBattLevel(long voltage) {
  int8_t level = 0;
  switch (data.bat) {
    case 1:
      level = map(voltage, battery_min, battery_max, 0, 99);
      level = constrain(level, 0, 99);
      mtrx.clear();
      printIcon(&mtrx, 0, 0, 4);
      printDig(&mtrx, 8 + 0, 1, level / 10);
      printDig(&mtrx, 8 + 4, 1, level % 10);
      mtrx.update();
      break;
    case 2:
      mtrx.clear();
      printDig(&mtrx, 0, 1, voltage / 1000);
      printDig(&mtrx, 4, 1, (voltage % 1000) / 100);
      printDig(&mtrx, 8 + 0, 1, (voltage % 100) / 10);
      printDig(&mtrx, 8 + 4, 1, (voltage % 100) % 10);
      mtrx.update();
      delay(2000);
      mtrx.clear();
      break;
  }
}

void changeMenuParam(int8_t menu, int8_t val) {
  disp_tmr.setTimeout(returnFromMenu, 5000);  // обновляем таймер
  mtrx.clear();                               // очищаем экран

  switch (menu) {
    case 1:            // яркость
      changeBri(val);  // сохраняем новое значение яркости
      break;
    case 2:  // громкость звуков
      changeVol(val);
      break;
    case 3:  // мелодия окончания
      changeMel(val);
      break;

    case 4:              // темп мелодий
      changeTempo(val);  // сохраняем новое значение темпа
      break;

    case 5:  // отображение заряда
      changeBat(val);
      break;
  }
  showMenu(menu);
}

// колбэк для выхода из меню по таймауту
void returnFromMenu() {
  lastUsedMenu = inMenu;  // запоминаем последнее использованное
  inMenu = 0;
}

void showIconNum(uint8_t icon, uint8_t val) {
  mtrx.clear();
  printIcon(&mtrx, 0, 0, icon);
  printDig(&mtrx, 8, 1, val / 10);
  printDig(&mtrx, 12, 1, val % 10);
  mtrx.update();
}

void drawDualEditor() {
  if (editField == 0) showIconNum(8, data.dual ? 1 : 0);
  else showClock(editField == 1 ? data.sec : data.secB, editField == 1 ? 0 : 1);
}

void nudgeDualEditor(int8_t dir) {
  if (editField == 0) {
    int8_t v = (int8_t)data.dual + (dir > 0 ? 1 : -1);
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    data.dual = (uint8_t)v;
    if (mpu.update(0)) fallDir = mpu.getDir();
    applyFallInterval();
  } else {
    int16_t* sec = (editField == 1) ? &data.sec : &data.secB;
    int32_t v = (int32_t)(*sec) + dir;
    if (v < 0) v = 0;
    if (v > 32767) v = 32767;
    *sec = (int16_t)v;
    // песок в редакторе стоит; интервал обновится при выходе
  }
  memory.update();
  editorDirty = 1;
}

void beginEditor(uint8_t kind, uint8_t menu) {
#ifdef SOUND_PIN
  StopMelody();
#endif
  disp_tmr.stop();  // пока правим поля, меню само не закрывается
  inMenu = 0;
  editKind = kind;
  editField = 0;
  editMenu = menu;
  editorDirty = 0;
}

void enterDualEdit() {
  beginEditor(1, MENU_DUAL);
  if (mpu.update(0)) fallDir = mpu.getDir();
  drawDualEditor();
}

void exitEditor() {
  metroHalt();
  uint8_t back = editMenu ? editMenu : 1;
  editKind = 0;
  editField = 0;
  editorDirty = 0;
  if (mpu.update(0)) fallDir = mpu.getDir();
  applyFallInterval();
  enterMenu(back);
}

void serviceDualEditor() {
  if (!editorDirty) return;
  editorDirty = 0;
  drawDualEditor();
}

void nudgeEditor(int8_t dir) {
  if (editKind == 2) nudgeMetroEditor(dir);
  else nudgeDualEditor(dir);
}

void editorButtons() {
  if (dbl.hold()) {
    exitEditor();
    return;
  }
  if (dbl.click()) {
    editField++;
    if (editField > 2) editField = 0;
    editorDirty = 1;
    return;
  }
  // как на часах: клик ±1, удержание ±10. На вкл/выкл и размере шаг всё равно 1
  if (up.click()) nudgeEditor(1);
  else if (up.hold() || up.step()) nudgeEditor(10);
  if (down.click()) nudgeEditor(-1);
  else if (down.hold() || down.step()) nudgeEditor(-10);
}

// обработчик кнопок
void buttons() {
  up.tick();
  down.tick();
  dbl.tick(up, down);

  if (editKind) {
    editorButtons();
    return;
  }

  if (!inMenu) {
    if (dbl.hold()) enterMenu(lastUsedMenu ? lastUsedMenu : 1);
    if (dbl.click()) resetSand();

    // останавливаем проигрывание мелодии при нажатии на любую кнопку
    if (up.click()) isMelodyPlaying() ? StopMelody() : changeTime(1);
    if (up.step()) changeTime(10);
    if (up.hold()) changeTime(10);

    // останавливаем проигрывание мелодии при нажатии на любую кнопку
    if (down.click()) isMelodyPlaying() ? StopMelody() : changeTime(-1);
    if (down.step()) changeTime(-10);
    if (down.hold()) changeTime(-10);
  } else {
    // два времени и метроном открываются в свой экран; клик одной кнопки тоже входит.
    // На остальных пунктах обе кнопки по-прежнему закрывают меню.
    bool openEditor = inMenu == MENU_DUAL || inMenu == MENU_METRO;
    if (openEditor && (dbl.click() || up.click() || down.click())) {
      if (inMenu == MENU_DUAL) enterDualEdit();
      else enterMetroEdit();
      return;
    }
    if (dbl.click()) returnFromMenu();
    if (up.hold()) enterMenu(inMenu + 1);
    if (down.hold()) enterMenu(inMenu - 1);
    if (up.click()) changeMenuParam(inMenu, 1);
    if (down.click()) changeMenuParam(inMenu, -1);
  }
}

void step() {
  uint16_t prd = 255 - mpu.getMag();
  prd = constrain(prd, 15, 90);
  if (mpu.update(prd)) {
    trackSide();
    mtrx.clear();
    box.step(mpu.getAngle() + 45);
    mtrx.update();
  }
}

void fall() {
  if (fall_tmr) {
    bool pushed = 0;
    if (mpu.getDir() > 0) {
      if (box.buf.get(7, 7) && !box.buf.get(8, 8)) {
        box.buf.set(7, 7, 0);
        box.buf.set(8, 8, 1);
        box.setCallback(7, 7, 0);
        box.setCallback(8, 8, 1);
        pushed = 1;
      }
    } else {
      if (box.buf.get(8, 8) && !box.buf.get(7, 7)) {
        box.buf.set(8, 8, 0);
        box.buf.set(7, 7, 1);
        box.setCallback(8, 8, 0);
        box.setCallback(7, 7, 1);
        pushed = 1;
      }
    }

    if (pushed) {
      runFlag = 0;
      mtrx.update();
      onSandPush();
    } else {
      if (!runFlag) {
        runFlag = 1;
        onSandStop();
      }
    }
  }
}

// Главный цикл
void loop() {
  // щелчок раньше перерисовки, только пока открыт экран метронома
  bool beat = 0;
  if (editKind == 2) beat = metroPoll();

  memory.tick();
  disp_tmr.tick();
#ifdef SOUND_PIN
  if (editKind != 2) soundsTick();
#endif
  buttons();

  if (editKind == 2) {
    serviceMetroEditor(beat);
    return;
  }
  if (editKind) {
    serviceDualEditor();
    return;
  }

  // список меню не крутит маятник; мигание только показывает, что метроном включён
  if (inMenu == MENU_METRO && data.metro) {
    static uint8_t hintPhase = 0xff;
    uint8_t phase = (millis() / 400) & 1;
    if (phase != hintPhase) {
      hintPhase = phase;
      mtrx.clear();
      showMenu(MENU_METRO);
    }
  }

  if (!disp_tmr.state()) {
    step();
    fall();
  }
}

unsigned long readVcc() {  //функция чтения внутреннего опорного напряжения, универсальная (для всех ардуин)
#if defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
  ADMUX = _BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
#elif defined(__AVR_ATtiny24__) || defined(__AVR_ATtiny44__) || defined(__AVR_ATtiny84__)
  ADMUX = _BV(MUX5) | _BV(MUX0);
#elif defined(__AVR_ATtiny25__) || defined(__AVR_ATtiny45__) || defined(__AVR_ATtiny85__)
  ADMUX = _BV(MUX3) | _BV(MUX2);
#else
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
#endif
  delay(2);             // Wait for Vref to settle
  ADCSRA |= _BV(ADSC);  // Start conversion
  while (bit_is_set(ADCSRA, ADSC))
    ;                   // measuring
  uint8_t low = ADCL;   // must read ADCL first - it then locks ADCH
  uint8_t high = ADCH;  // unlocks both
  long result = (high << 8) | low;
  result = my_vcc_const * 1023 * 1000 / result;  // расчёт реального VCC
  return result;                                 // возвращает VCC
}