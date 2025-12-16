#include "freertos.h"
#include "pins.h"
#include "vector_draw.h"
#include <Arduino.h>
#include <stdio.h>

// External variables from main.cpp
extern volatile int32_t encoderValue;

// Task Handles
static TaskHandle_t s_serialOutputTaskHandle = nullptr;
static TaskHandle_t s_guiTaskHandle = nullptr;

// --- Snake Game Defines ---
#define SNAKE_GRID_SIZE 20
#define SNAKE_MAX_LEN 100
typedef struct {
    int8_t x;
    int8_t y;
} Point;

static Point snake_body[SNAKE_MAX_LEN];
static int snake_len = 3;
static Point snake_food;
static int snake_dir = 3; // 0:UP, 1:DOWN, 2:LEFT, 3:RIGHT
static int game_over = 0;
static uint32_t last_game_tick = 0;
static int game_score = 0;
static int8_t Game_Input_Dir = 3;

// Touch Pins
#define TOUCH_UP    4
#define TOUCH_DOWN  6
#define TOUCH_LEFT  5
#define TOUCH_RIGHT 7
#define TOUCH_THRESHOLD 35000 

// Task Functions
static void serialOutputTask(void* pvParameters);
static void guiTask(void* pvParameters);

void initTasks() {
  // Create Serial Output Task
  if (!s_serialOutputTaskHandle) {
    xTaskCreatePinnedToCore(
      serialOutputTask,
      "SerialOutputTask",
      2048,
      nullptr,
      1,
      &s_serialOutputTaskHandle,
      1 // Core 1
    );
  }

  // Create GUI Task
  if (!s_guiTaskHandle) {
    xTaskCreatePinnedToCore(
      guiTask,
      "GuiTask",
      4096, // Larger stack for GUI
      nullptr,
      1,
      &s_guiTaskHandle,
      1 // Core 1
    );
  }
}

// --- GUI Logic ---

enum UI_State {
    UI_MENU_MAIN,
    UI_OSCILLOSCOPE,
    UI_SNAKE,
    UI_ABOUT
};

static const char* main_menu_items[] = {
    "Oscilloscope",
    "Snake Game",
    "Settings",
    "About"
};
static const int main_menu_count = 4;

// --- Snake Game Logic ---
void Init_Snake_Game(void) {
    snake_len = 3;
    snake_body[0].x = 10; snake_body[0].y = 10;
    snake_body[1].x = 10; snake_body[1].y = 9;
    snake_body[2].x = 10; snake_body[2].y = 8;
    
    snake_dir = 3; // RIGHT
    Game_Input_Dir = 3;
    
    // Random Food
    snake_food.x = rand() % SNAKE_GRID_SIZE;
    snake_food.y = rand() % SNAKE_GRID_SIZE;
    
    game_over = 0;
    game_score = 0;
    last_game_tick = millis();
}

void Update_Snake_Game(void) {
    if(game_over) return;
    
    // Update Direction from Input
    // Prevent 180 degree turns
    if(Game_Input_Dir == 0 && snake_dir != 1) snake_dir = 0;
    if(Game_Input_Dir == 1 && snake_dir != 0) snake_dir = 1;
    if(Game_Input_Dir == 2 && snake_dir != 3) snake_dir = 2;
    if(Game_Input_Dir == 3 && snake_dir != 2) snake_dir = 3;
    
    if(millis() - last_game_tick > 200) { // 200ms speed
        last_game_tick = millis();
        
        // Move Body
        for(int i=snake_len-1; i>0; i--) {
            snake_body[i] = snake_body[i-1];
        }
        
        // Move Head
        if(snake_dir == 0) snake_body[0].y++; // UP
        if(snake_dir == 1) snake_body[0].y--; // DOWN
        if(snake_dir == 2) snake_body[0].x--; // LEFT
        if(snake_dir == 3) snake_body[0].x++; // RIGHT
        
        // Check Wall Collision
        if(snake_body[0].x < 0 || snake_body[0].x >= SNAKE_GRID_SIZE ||
           snake_body[0].y < 0 || snake_body[0].y >= SNAKE_GRID_SIZE) {
            game_over = 1;
        }
        
        // Check Self Collision
        for(int i=1; i<snake_len; i++) {
            if(snake_body[0].x == snake_body[i].x && snake_body[0].y == snake_body[i].y) {
                game_over = 1;
            }
        }
        
        // Check Food
        if(snake_body[0].x == snake_food.x && snake_body[0].y == snake_food.y) {
            if(snake_len < SNAKE_MAX_LEN) {
                snake_len++;
                game_score += 10;
            }
            // New Food
            snake_food.x = rand() % SNAKE_GRID_SIZE;
            snake_food.y = rand() % SNAKE_GRID_SIZE;
        }
    }
}

static void guiTask(void* pvParameters) {
    UI_State ui_state = UI_MENU_MAIN;
    int menu_index = 0;
    int last_menu_index = -1;
    int32_t last_encoder = 0;
    
    // Debounce for button
    int last_btn_state = HIGH;
    unsigned long last_btn_time = 0;
    
    // Oscilloscope state
    int32_t osc_last_val = -999999;

    for (;;) {
        // 1. Handle Input
        int32_t current_encoder = encoderValue / 4;
        int16_t enc_delta = current_encoder - last_encoder;
        last_encoder = current_encoder;
        
        int btn_state = digitalRead(EN_S);
        bool btn_pressed = false;
        
        if (btn_state == LOW && last_btn_state == HIGH && (millis() - last_btn_time > 50)) {
            btn_pressed = true;
            last_btn_time = millis();
        }
        last_btn_state = btn_state;

        // Touch Input for Snake
        if (ui_state == UI_SNAKE) {
            if (touchRead(TOUCH_UP) > TOUCH_THRESHOLD) Game_Input_Dir = 0;
            if (touchRead(TOUCH_DOWN) > TOUCH_THRESHOLD) Game_Input_Dir = 1;
            if (touchRead(TOUCH_LEFT) > TOUCH_THRESHOLD) Game_Input_Dir = 2;
            if (touchRead(TOUCH_RIGHT) > TOUCH_THRESHOLD) Game_Input_Dir = 3;
        }

        // 2. State Machine
        bool rebuild = false;

        if (ui_state == UI_MENU_MAIN) {
            // Navigation
            if (enc_delta != 0) {
                menu_index += enc_delta;
                if (menu_index < 0) menu_index = main_menu_count - 1;
                if (menu_index >= main_menu_count) menu_index = 0;
                rebuild = true;
            }
            
            // Selection
            if (btn_pressed) {
                if (menu_index == 0) {
                    ui_state = UI_OSCILLOSCOPE;
                    osc_last_val = -999999; // Force redraw
                } else if (menu_index == 1) {
                    ui_state = UI_SNAKE;
                    Init_Snake_Game();
                } else if (menu_index == 3) {
                    ui_state = UI_ABOUT;
                }
                rebuild = true;
            }
            
            // Render Main Menu
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(0, 0, 2047, 2047); // Full Screen Border
                
                // 5 lines layout -> Spacing ~400
                // Top-Left alignment
                int start_y = 1600; 
                int spacing = 400;
                int scale = 40; // Large text
                
                for (int i = 0; i < main_menu_count; i++) {
                    int y = start_y - (i * spacing);
                    if (i == menu_index) {
                        DRAW_AddString(">", 0, 50, y, scale, scale);
                    }
                    DRAW_AddString(main_menu_items[i], 0, 250, y, scale, scale);
                }
                last_menu_index = menu_index;
            }
            
        } else if (ui_state == UI_OSCILLOSCOPE) {
            // Exit
            if (btn_pressed) {
                ui_state = UI_MENU_MAIN;
                rebuild = true;
                last_menu_index = -1; // Force menu redraw
            }
            
            // Render Oscilloscope (Only if changed)
            if (current_encoder != osc_last_val || rebuild) {
                osc_last_val = current_encoder;
                
                DRAW_Clear();
                DRAW_AddRect(50, 50, 1948, 1948);
                DRAW_AddString("OSCILLOSCOPE", 0, 530, 1800, 15, 15);
                
                char buf[32];
                sprintf(buf, "VAL: %ld", current_encoder);
                DRAW_AddString(buf, 0, 420, 1000, 20, 20);
                
                DRAW_AddString("[BTN] TO EXIT", 0, 750, 250, 7, 7);
            }
            
        } else if (ui_state == UI_SNAKE) {
            // Exit
            if (btn_pressed) {
                ui_state = UI_MENU_MAIN;
                rebuild = true;
                last_menu_index = -1;
            }
            
            Update_Snake_Game();
            
            // Always redraw game
            DRAW_Clear();
            DRAW_AddRect(0, 0, 2047, 2047);
            
            // Draw Snake
            int cell_size = 2048 / SNAKE_GRID_SIZE;
            for(int i=0; i<snake_len; i++) {
                int x = snake_body[i].x * cell_size + (cell_size/2);
                int y = snake_body[i].y * cell_size + (cell_size/2);
                int half_size = (cell_size / 2) - 5;
                DRAW_AddRect(x - half_size, y - half_size, 2*half_size, 2*half_size);
            }
            
            // Draw Food
            int fx = snake_food.x * cell_size + (cell_size/2);
            int fy = snake_food.y * cell_size + (cell_size/2);
            int f_half = (cell_size / 2) - 10;
            DRAW_AddLine(fx - f_half, fy - f_half, fx + f_half, fy + f_half);
            DRAW_AddLine(fx - f_half, fy + f_half, fx + f_half, fy - f_half);
            
            if(game_over) {
                DRAW_AddString("GAME OVER", 0, 500, 1100, 20, 20);
                char score_str[32];
                sprintf(score_str, "SCORE: %d", game_score);
                DRAW_AddString(score_str, 0, 600, 800, 15, 15);
            }

        } else if (ui_state == UI_ABOUT) {
             if (btn_pressed) {
                ui_state = UI_MENU_MAIN;
                rebuild = true;
                last_menu_index = -1;
            }
            
            if (rebuild || last_menu_index == -1) {
                DRAW_Clear();
                DRAW_AddRect(50, 50, 1948, 1948);
                DRAW_AddString("ABOUT", 0, 780, 1800, 17, 17);
                DRAW_AddString("ESP32 VECTOR", 0, 610, 1300, 12, 12);
                DRAW_AddString("DISPLAY SYSTEM", 0, 540, 1050, 12, 12);
                DRAW_AddString("V1.0", 0, 885, 800, 12, 12);
                last_menu_index = 0; // Mark as drawn
            }
        }

        // Update animations and Render
        DRAW_Update();
        DRAW_Render();

        vTaskDelay(pdMS_TO_TICKS(40)); // 25Hz update rate
    }
}

static void serialOutputTask(void* pvParameters) {
  unsigned long lastOutputTime = 0;
  const unsigned long outputInterval = 200; // 200ms interval
  
  for (;;) {
    if (millis() - lastOutputTime >= outputInterval) {
      lastOutputTime = millis();
      Serial.printf("Touch(Cap): U=%d D=%d L=%d R=%d\n", 
        touchRead(TOUCH_UP), 
        touchRead(TOUCH_DOWN), 
        touchRead(TOUCH_LEFT), 
        touchRead(TOUCH_RIGHT));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}