#include <SPI.h>
#include <U8g2lib.h>

U8G2_SH1106_128X64_NONAME_1_4W_HW_SPI u8g2(U8G2_R2, 10, 9, 8);

// Hardware pins
const int buttons[]  = { 6, 5, 7, A0 };
const int lights[]   = { A2, A3, A4, A5 };
const int NUM_BTN    = 4;
const int BUZZER     = 3;

// One note per button (C4, E4, G4, B4)
const int BTN_FREQ[NUM_BTN] = { 523, 659, 784, 988 };
const char* BTN_NAME[NUM_BTN] = { "C", "E", "G", "B" };

// Constants
const int MAX_SEQ        = 20;
const int BASE_NOTE_DUR  = 400;   // ms each note plays during SHOW
const int BASE_GAP       = 100;   // ms silence between shown notes
const int MAX_WAIT_MS    = 5000;  // input timeout per note

// Difficulty
// diffLevel 1–5:
//   length starts at 3, increases every level-up
//   timing tolerance shrinks as difficulty rises
struct Difficulty {
  int  seqLen;          // notes in the sequence
  int  timingTolerancePct; // allowed % deviation in inter-note timing (100 = anything goes)
};

Difficulty getDifficulty(int level) {
  int l = constrain(level, 1, 10);
  return { 2 + l, max(20, 100 - (l - 1) * 10) };
}

// Game States
enum State { START, SHOW_SEQUENCE, PLAYER_INPUT, SCORE, GAME_OVER };
State gameState = START;

int  sequence[MAX_SEQ];         // button indices 0-3
long seqIntervals[MAX_SEQ];     // gap (ms) *before* each note during generation (0 for first)
int  seqLen       = 0;

int  playerInput[MAX_SEQ];
long playerIntervals[MAX_SEQ];  // time between consecutive button presses
int  playerCount  = 0;

int  diffLevel    = 1;
int  lives        = 3;
int  totalScore   = 0;

bool prevPressed[NUM_BTN] = {};

// Display function
void updateDisplay(const char* l1, const char* l2 = "", const char* l3 = "", const char* l4 = "") {
  u8g2.firstPage();
  do {
    u8g2.drawStr(0, 15, l1);
    u8g2.drawStr(0, 30, l2);
    u8g2.drawStr(0, 45, l3);
    u8g2.drawStr(0, 60, l4);
  } while (u8g2.nextPage());
}

void allLightsOff() {
  for (int i = 0; i < NUM_BTN; i++) digitalWrite(lights[i], LOW);
}

// Returns bitmask of newly-pressed buttons this frame
// Also updates prevPressed[]
byte readNewPresses() {
  byte mask = 0;
  for (int i = 0; i < NUM_BTN; i++) {
    bool now = (digitalRead(buttons[i]) == LOW);
    if (now && !prevPressed[i]) mask |= (1 << i);
    prevPressed[i] = now;
  }
  return mask;
}

bool allPressed() {
  for (int i = 0; i < NUM_BTN; i++)
    if (digitalRead(buttons[i]) == HIGH) return false;
  return true;
}

void flashAll(int times, int ms = 80) {
  for (int t = 0; t < times; t++) {
    for (int i = 0; i < NUM_BTN; i++) digitalWrite(lights[i], HIGH);
    delay(ms);
    allLightsOff();
    delay(ms);
  }
}

void playNote(int btnIdx, int durationMs) {
  tone(BUZZER, BTN_FREQ[btnIdx]);
  digitalWrite(lights[btnIdx], HIGH);
  delay(durationMs);
  noTone(BUZZER);
  digitalWrite(lights[btnIdx], LOW);
}

// ── Generate sequence ─────────────────────────────────────
// Stores a random sequence of button indices.
// seqIntervals[i] holds the generated gap before note i
// (so we can later evaluate rhythm accuracy).
void generateSequence(int len) {
  seqLen = len;
  seqIntervals[0] = 0;
  for (int i = 0; i < len; i++) {
    sequence[i] = random(NUM_BTN);
    if (i > 0) {
      // vary gap between 150–500 ms to create a rhythm
      seqIntervals[i] = random(150, 501);
    }
  }
}

// Show player sequence
void showSequence() {
  for (int i = 0; i < seqLen; i++) {
    delay(i == 0 ? 600 : seqIntervals[i]);  // respect generated rhythm
    playNote(sequence[i], BASE_NOTE_DUR);
  }
  delay(400);
}

// Evaluate Score
// Returns 0–100:
//   pitch component (50 pts): correct notes in order
//   rhythm component (50 pts): timing within tolerance
int evaluatePerformance(const Difficulty& d) {
  // Pitch score
  int pitchCorrect = 0;
  for (int i = 0; i < seqLen; i++) {
    if (playerInput[i] == sequence[i]) pitchCorrect++;
  }
  int pitchScore = (pitchCorrect * 50) / seqLen;

  // Rhythm score — compare relative inter-note gaps
  // Skip index 0 (no previous note), evaluate indices 1..seqLen-1
  int rhythmScore = 50; // full marks if sequence too short to measure
  if (seqLen > 1) {
    int rhythmCorrect = 0;
    int rhythmTotal   = seqLen - 1;
    for (int i = 1; i < seqLen; i++) {
      long expected = seqIntervals[i];
      long actual   = playerIntervals[i];
      if (expected == 0) { rhythmCorrect++; continue; }
      int pctDiff = abs((int)(((actual - expected) * 100L) / expected));
      if (pctDiff <= d.timingTolerancePct) rhythmCorrect++;
    }
    rhythmScore = (rhythmCorrect * 50) / rhythmTotal;
  }

  return pitchScore + rhythmScore;
}

void setup() {
  Serial.begin(9600);
  // Generates randomness from empty pin
  randomSeed(analogRead(A1));

  for (int i = 0; i < NUM_BTN; i++) {
    pinMode(buttons[i], INPUT_PULLUP);
    pinMode(lights[i],  OUTPUT);
  }
  pinMode(BUZZER, OUTPUT);

  u8g2.begin();
  u8g2.setFont(u8g2_font_ncenB08_tr);
}

// ── Main loop ─────────────────────────────────────────────
void loop() {

  // Global restart: all 4 buttons simultaneously
  if (allPressed()) {
    allLightsOff();
    noTone(BUZZER);
    diffLevel   = 1;
    lives       = 3;
    totalScore  = 0;
    playerCount = 0;
    gameState   = START;
    delay(600);
    return;
  }

  switch (gameState) {

    // ── START ─────────────────────────────────────────────
    case START: {
      updateDisplay("Note Sequencer", "Repeat the melody!", "Press any button", "to begin");
      byte p = readNewPresses();
      if (p) {
        delay(200);
        gameState = SHOW_SEQUENCE;
      }
      break;
    }

    // ── SHOW_SEQUENCE ─────────────────────────────────────
    case SHOW_SEQUENCE: {
      Difficulty d = getDifficulty(diffLevel);

      char buf[22];
      snprintf(buf, sizeof(buf), "Level %d  Lives: %d", diffLevel, lives);
      updateDisplay(buf, "Watch carefully...");
      delay(800);

      generateSequence(d.seqLen);
      showSequence();

      playerCount = 0;
      memset(playerIntervals, 0, sizeof(playerIntervals));

      updateDisplay("Your turn!", "Repeat the melody");
      delay(300);
      gameState = PLAYER_INPUT;
      break;
    }

    case PLAYER_INPUT: {
      static long lastPressTime = 0;

      // Timeout: too slow treat remaining notes as wrong
      if (millis() - lastPressTime > MAX_WAIT_MS && playerCount > 0) {
        // Fill remaining with an invalid index so scoring fails them
        while (playerCount < seqLen) {
          playerInput[playerCount]     = -1;
          playerIntervals[playerCount] = MAX_WAIT_MS;
          playerCount++;
        }
        gameState = SCORE;
        break;
      }

      byte p = readNewPresses();
      if (!p) break;

      // Find which button was pressed
      int btn = -1;
      for (int i = 0; i < NUM_BTN; i++) {
        if (p & (1 << i)) { btn = i; break; }
      }
      if (btn < 0) break;

      long now = millis();
      playerIntervals[playerCount] = (playerCount == 0) ? 0 : (now - lastPressTime);
      lastPressTime = now;
      playerInput[playerCount] = btn;

      // Audible+visual feedback
      playNote(btn, 200);

      // Show progress on display
      char prog[22];
      snprintf(prog, sizeof(prog), "Note %d / %d", playerCount + 1, seqLen);
      updateDisplay("Your turn!", prog);

      playerCount++;
      if (playerCount >= seqLen) {
        gameState = SCORE;
      }
      break;
    }

    case SCORE: {
      Difficulty d  = getDifficulty(diffLevel);
      int score     = evaluatePerformance(d);
      bool passed   = (score >= 60);  // 60/100 threshold to advance
      totalScore   += score;

      char line1[22], line2[22], line3[22];
      snprintf(line1, sizeof(line1), "Score: %d/100", score);

      if (passed) {
        snprintf(line2, sizeof(line2), "Great! Lvl %d->%d", diffLevel, diffLevel + 1);
        diffLevel++;
        flashAll(3, 60);
        // Victory song
        for (int i = 0; i < NUM_BTN; i++) { playNote(i, 120); delay(30); }
        snprintf(line3, sizeof(line3), "Total: %d", totalScore);
        updateDisplay("Passed!", line1, line2, line3);
        delay(1800);
        gameState = SHOW_SEQUENCE;
      } else {
        lives--;
        snprintf(line2, sizeof(line2), "Lives left: %d", lives);
        snprintf(line3, sizeof(line3), "Total: %d", totalScore);
        // Play a descending fail tone
        for (int i = NUM_BTN - 1; i >= 0; i--) { playNote(i, 100); delay(20); }
        updateDisplay("Try again...", line1, line2, line3);
        delay(2000);
        gameState = (lives <= 0) ? GAME_OVER : SHOW_SEQUENCE;
      }
      break;
    }

    // ── GAME_OVER ─────────────────────────────────────────
    case GAME_OVER: {
      char buf[22];
      snprintf(buf, sizeof(buf), "Final: %d pts", totalScore);
      updateDisplay("Game Over!", buf, "Hold all buttons", "to restart");
      flashAll(2, 150);
      delay(1000);
      break;
    }
  }
}