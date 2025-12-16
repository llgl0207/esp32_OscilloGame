import tkinter as tk
from tkinter import ttk
import serial
import serial.tools.list_ports
import threading
import time
import re

# ================= 配置区域 =================
SERIAL_PORT = 'COM3'  # 请根据实际情况修改串口号
BAUD_RATE = 115200
# ===========================================

class GamepadVisualizer:
    def __init__(self, root):
        self.root = root
        self.root.title("ESP32 Gamepad Monitor")
        self.root.geometry("800x400")
        
        self.serial_conn = None
        self.running = True
        
        # 数据变量
        self.j1_x = 2048
        self.j1_y = 2048
        self.j1_sw = 1
        self.j2_x = 2048
        self.j2_y = 2048
        self.j2_sw = 1
        self.btn_a = 1
        self.btn_b = 1
        self.dpad_up = 0
        self.dpad_left = 0
        self.dpad_down = 0
        self.dpad_right = 0

        self.setup_ui()
        self.start_serial_thread()

    def setup_ui(self):
        # 主容器
        main_frame = ttk.Frame(self.root, padding="20")
        main_frame.pack(fill=tk.BOTH, expand=True)

        # 标题
        ttk.Label(main_frame, text="ESP32 Gamepad Status", font=("Arial", 16, "bold")).pack(pady=10)

        # 状态显示区域
        status_frame = ttk.Frame(main_frame)
        status_frame.pack(fill=tk.BOTH, expand=True)

        # 左摇杆区域
        self.canvas_j1 = tk.Canvas(status_frame, width=200, height=200, bg="white", highlightthickness=1, highlightbackground="#ccc")
        self.canvas_j1.grid(row=0, column=0, padx=20)
        ttk.Label(status_frame, text="Joystick 1 (Left)").grid(row=1, column=0)
        
        # 右摇杆区域
        self.canvas_j2 = tk.Canvas(status_frame, width=200, height=200, bg="white", highlightthickness=1, highlightbackground="#ccc")
        self.canvas_j2.grid(row=0, column=1, padx=20)
        ttk.Label(status_frame, text="Joystick 2 (Right)").grid(row=1, column=1)

        # 按键区域
        btn_frame = ttk.Frame(status_frame)
        btn_frame.grid(row=0, column=2, padx=20, sticky="n")
        
        self.lbl_j1_sw = self.create_indicator(btn_frame, "J1 SW")
        self.lbl_j2_sw = self.create_indicator(btn_frame, "J2 SW")
        self.lbl_btn_a = self.create_indicator(btn_frame, "Button A")
        self.lbl_btn_b = self.create_indicator(btn_frame, "Button B")

        # D-Pad 区域
        dpad_frame = ttk.Frame(status_frame)
        dpad_frame.grid(row=0, column=3, padx=20, sticky="n")
        ttk.Label(dpad_frame, text="D-Pad").pack(pady=5)
        
        # 十字布局
        dpad_grid = ttk.Frame(dpad_frame)
        dpad_grid.pack()
        
        self.lbl_dpad_up = tk.Label(dpad_grid, text="▲", fg="gray", font=("Arial", 20))
        self.lbl_dpad_up.grid(row=0, column=1)
        
        self.lbl_dpad_left = tk.Label(dpad_grid, text="◀", fg="gray", font=("Arial", 20))
        self.lbl_dpad_left.grid(row=1, column=0)
        
        self.lbl_dpad_down = tk.Label(dpad_grid, text="▼", fg="gray", font=("Arial", 20))
        self.lbl_dpad_down.grid(row=2, column=1)
        
        self.lbl_dpad_right = tk.Label(dpad_grid, text="▶", fg="gray", font=("Arial", 20))
        self.lbl_dpad_right.grid(row=1, column=2)

        # 原始数据显示
        self.raw_data_label = ttk.Label(main_frame, text="Waiting for data...", font=("Consolas", 10))
        self.raw_data_label.pack(pady=20)

        # 绘制初始摇杆
        self.draw_joystick(self.canvas_j1, 2048, 2048, 1)
        self.draw_joystick(self.canvas_j2, 2048, 2048, 1)

    def create_indicator(self, parent, text):
        frame = ttk.Frame(parent)
        frame.pack(pady=5, fill=tk.X)
        lbl = tk.Label(frame, text="●", fg="gray", font=("Arial", 20))
        lbl.pack(side=tk.LEFT)
        ttk.Label(frame, text=text).pack(side=tk.LEFT, padx=5)
        return lbl

    def draw_joystick(self, canvas, x, y, sw):
        canvas.delete("all")
        w = 200
        h = 200
        center_x = w // 2
        center_y = h // 2
        
        # 绘制背景十字
        canvas.create_line(center_x, 0, center_x, h, fill="#eee")
        canvas.create_line(0, center_y, w, center_y, fill="#eee")
        
        # 映射坐标 (0-4095 -> 0-200)
        # 注意：通常摇杆Y轴向上是变小还是变大取决于安装，这里假设0是上，4095是下
        pos_x = (x / 4095) * w
        pos_y = (y / 4095) * h
        
        # 绘制摇杆点
        color = "yellow" if sw == 0 else "blue"
        radius = 10
        canvas.create_oval(pos_x - radius, pos_y - radius, pos_x + radius, pos_y + radius, fill=color, outline="")
        
        # 显示坐标文字
        canvas.create_text(10, 10, anchor="nw", text=f"X:{x}\nY:{y}", font=("Arial", 8))

    def update_indicator(self, label, pressed):
        if pressed:
            label.config(fg="yellow") # 按下变黄
        else:
            label.config(fg="gray")  # 松开变灰

    def parse_data(self, line):
        try:
            # 格式: J1:2048,2048,1 | J2:2048,2048,1 | A:1 B:1 | Dpad:0,0,0,0
            parts = line.split('|')
            if len(parts) >= 3:
                # Part 1: J1: 2048, 2048, 1
                p1 = parts[0].split(':')[1].split(',')
                self.j1_x = int(p1[0])
                self.j1_y = int(p1[1])
                self.j1_sw = int(p1[2])
                
                # Part 2: J2: 2048, 2048, 1
                p2 = parts[1].split(':')[1].split(',')
                self.j2_x = int(p2[0])
                self.j2_y = int(p2[1])
                self.j2_sw = int(p2[2])
                
                # Part 3: A:1 B:1
                p3 = parts[2].strip().split() # ['A:1', 'B:1']
                self.btn_a = int(p3[0].split(':')[1])
                self.btn_b = int(p3[1].split(':')[1])
                
                # Part 4: Dpad:0,0,0,0 (Optional for backward compatibility)
                if len(parts) >= 4:
                    p4 = parts[3].split(':')[1].split(',')
                    self.dpad_up = int(p4[0])
                    self.dpad_left = int(p4[1])
                    self.dpad_down = int(p4[2])
                    self.dpad_right = int(p4[3])
                
                return True
        except Exception as e:
            print(f"Parse error: {e}")
        return False

    def update_gui(self):
        self.draw_joystick(self.canvas_j1, self.j1_x, self.j1_y, self.j1_sw)
        self.draw_joystick(self.canvas_j2, self.j2_x, self.j2_y, self.j2_sw)
        
        self.update_indicator(self.lbl_j1_sw, self.j1_sw == 0)
        self.update_indicator(self.lbl_j2_sw, self.j2_sw == 0)
        self.update_indicator(self.lbl_btn_a, self.btn_a == 0)
        self.update_indicator(self.lbl_btn_b, self.btn_b == 0)
        
        self.update_indicator(self.lbl_dpad_up, self.dpad_up == 1)
        self.update_indicator(self.lbl_dpad_left, self.dpad_left == 1)
        self.update_indicator(self.lbl_dpad_down, self.dpad_down == 1)
        self.update_indicator(self.lbl_dpad_right, self.dpad_right == 1)
        
        self.root.after(50, self.update_gui) # 20Hz 刷新 UI

    def serial_loop(self):
        print(f"Connecting to {SERIAL_PORT}...")
        while self.running:
            try:
                if self.serial_conn is None or not self.serial_conn.is_open:
                    self.serial_conn = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
                    print("Connected!")
                
                if self.serial_conn.in_waiting:
                    line = self.serial_conn.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        self.raw_data_label.config(text=line)
                        if self.parse_data(line):
                            pass
                else:
                    time.sleep(0.01)
                    
            except serial.SerialException:
                print("Serial connection lost/failed. Retrying...")
                if self.serial_conn:
                    self.serial_conn.close()
                self.serial_conn = None
                time.sleep(2)
            except Exception as e:
                print(f"Error: {e}")
                time.sleep(1)

    def start_serial_thread(self):
        t = threading.Thread(target=self.serial_loop)
        t.daemon = True
        t.start()
        self.update_gui()

if __name__ == "__main__":
    root = tk.Tk()
    app = GamepadVisualizer(root)
    root.mainloop()
