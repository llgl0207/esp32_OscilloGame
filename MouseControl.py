import serial
import time
import threading
import pydirectinput
import pyautogui

# ================= 配置区域 =================
SERIAL_PORT = 'COM3'  # 串口号
BAUD_RATE = 115200
MOUSE_SPEED = 5      # 鼠标移动速度系数
SCROLL_SPEED = 80     # 滚轮速度系数
DEAD_ZONE = 20       # 摇杆死区 (防止漂移)
CENTER_VAL = 2048     # 摇杆中心值
# ===========================================

# 禁用 pyautogui 的自动防故障功能 (鼠标移到角落会触发异常)
pydirectinput.FAILSAFE = False
pydirectinput.PAUSE = 0 # 禁用默认延迟，防止操作积压导致卡顿和延迟
pyautogui.FAILSAFE = False # 同时也禁用 pyautogui 的

class MouseController:
    def __init__(self):
        self.serial_conn = None
        self.running = True
        
        # 鼠标按键状态
        self.left_click_last = 1
        self.right_click_last = 1
        
        # 摇杆按键状态 (用于双击检测)
        self.j1_sw_last = 1
        self.j2_sw_last = 1
        
        # WASD 状态
        self.keys_pressed = {'w': False, 'a': False, 's': False, 'd': False}
        
        # 摇杆中心值 (将在启动时校准)
        self.j1_center_x = CENTER_VAL
        self.j1_center_y = CENTER_VAL
        self.j2_center_x = CENTER_VAL
        self.j2_center_y = CENTER_VAL
        self.calibrated = False
        
        # 看门狗: 最后一次有效数据的时间
        self.last_data_time = time.time()

    def release_all_keys(self):
        """释放所有按键，防止卡死"""
        for key in self.keys_pressed:
            if self.keys_pressed[key]:
                pydirectinput.keyUp(key)
                self.keys_pressed[key] = False
        # 确保鼠标按键也释放
        if self.left_click_last == 0:
            pydirectinput.mouseUp(button='left')
            self.left_click_last = 1
        if self.right_click_last == 0:
            pydirectinput.mouseUp(button='right')
            self.right_click_last = 1

    def update_key(self, key, should_press):
        """更新键盘按键状态"""
        if should_press and not self.keys_pressed[key]:
            pydirectinput.keyDown(key)
            self.keys_pressed[key] = True
        elif not should_press and self.keys_pressed[key]:
            pydirectinput.keyUp(key)
            self.keys_pressed[key] = False

    def map_value(self, val, center_val, speed=MOUSE_SPEED):
        """将摇杆值映射为速度"""
        # 计算偏移量
        diff = val - center_val
        
        # 死区处理
        if abs(diff) < DEAD_ZONE:
            return 0
            
        # 映射到速度 (线性映射)
        # 归一化 (-2048 到 2048) -> (-1.0 到 1.0)
        norm = diff / 2048.0
        
        # 应用速度系数
        return int(norm * speed)

    def parse_and_act(self, line):
        try:
            # 格式: J1:2048,2048,1 | J2:2048,2048,1 | A:1 B:1 | Dpad:0,0,0,0
            parts = line.split('|')
            if len(parts) >= 3:
                # 获取 Joy1 数据 (左摇杆)
                # Part 1: J1: 2048, 2048, 1
                p1 = parts[0].split(':')[1].split(',')
                j1_x = int(p1[0])
                j1_y = int(p1[1])
                j1_sw = int(p1[2])

                # 获取 Joy2 数据 (右摇杆)
                # Part 2: J2: 2048, 2048, 1
                p2 = parts[1].split(':')[1].split(',')
                j2_x = int(p2[0])
                j2_y = int(p2[1])
                j2_sw = int(p2[2])
                
                # 如果未校准，则进行校准 (采集前20帧的平均值)
                if not self.calibrated:
                    if not hasattr(self, 'calib_samples'):
                        self.calib_samples = []
                    
                    self.calib_samples.append((j1_x, j1_y, j2_x, j2_y))
                    
                    if len(self.calib_samples) >= 20:
                        # 计算平均值
                        avg_j1_x = sum(s[0] for s in self.calib_samples) / 20
                        avg_j1_y = sum(s[1] for s in self.calib_samples) / 20
                        avg_j2_x = sum(s[2] for s in self.calib_samples) / 20
                        avg_j2_y = sum(s[3] for s in self.calib_samples) / 20
                        
                        self.j1_center_x = int(avg_j1_x)
                        self.j1_center_y = int(avg_j1_y)
                        self.j2_center_x = int(avg_j2_x)
                        self.j2_center_y = int(avg_j2_y)
                        
                        self.calibrated = True
                        print(f"Calibration Complete: J1({self.j1_center_x},{self.j1_center_y}) J2({self.j2_center_x},{self.j2_center_y})")
                    return # 校准期间不执行动作

                # 获取按键数据
                # Part 3: A:1 B:1
                p3 = parts[2].strip().split()
                btn_a = int(p3[0].split(':')[1]) # 左键
                btn_b = int(p3[1].split(':')[1]) # 右键

                # 1. 处理移动 (右摇杆控制鼠标)
                move_x = self.map_value(j2_x, self.j2_center_x)
                move_y = self.map_value(j2_y, self.j2_center_y) 
                
                if move_x != 0 or move_y != 0:
                    pydirectinput.moveRel(move_x, move_y, relative=True)

                # 2. 处理左键 (Button A)
                if btn_a == 0 and self.left_click_last == 1:
                    # 特殊功能: 按住右键(B)时按左键(A) -> E键 (背包)
                    if btn_b == 0:
                        pydirectinput.keyDown('e')
                        time.sleep(0.05)
                        pydirectinput.keyUp('e')
                    else:
                        pydirectinput.mouseDown(button='left')
                elif btn_a == 1 and self.left_click_last == 0:
                    pydirectinput.mouseUp(button='left')
                self.left_click_last = btn_a

                # 3. 处理右键 (Button B)
                if btn_b == 0 and self.right_click_last == 1:
                    pydirectinput.mouseDown(button='right')
                elif btn_b == 1 and self.right_click_last == 0:
                    pydirectinput.mouseUp(button='right')
                self.right_click_last = btn_b

                # 4. 处理 WASD (左摇杆)
                # 阈值判断 (超过死区即视为按下)
                threshold = DEAD_ZONE + 700
                
                # W/S (Y轴)
                # 向上推(数值变小) -> W
                self.update_key('w', j1_y < (self.j1_center_y - threshold))
                # 向下推(数值变大) -> S
                self.update_key('s', j1_y > (self.j1_center_y + threshold))
                
                # A/D (X轴)
                # 向左推(数值变小) -> A
                self.update_key('a', j1_x < (self.j1_center_x - threshold))
                # 向右推(数值变大) -> D
                self.update_key('d', j1_x > (self.j1_center_x + threshold))

                # 5. 特殊功能: 
                # Joy1 按下 -> 滚轮向上
                if j1_sw == 0 and self.j1_sw_last == 1:
                    pyautogui.scroll(100)

                # Joy2 按下 -> 
                #   如果 Joy1 已经按下 -> 空格 (Space)
                #   否则 -> 滚轮向下
                if j2_sw == 0 and self.j2_sw_last == 1:
                    if j1_sw == 0:
                        # 游戏通常需要按键持续一段时间才能识别
                        pydirectinput.keyDown('space')
                        time.sleep(0.05) # 保持 50ms
                        pydirectinput.keyUp('space')
                        # print("Action: Space (Jump)")
                    else:
                        pyautogui.scroll(-100)

                self.j1_sw_last = j1_sw
                self.j2_sw_last = j2_sw
                
                # 更新看门狗时间
                self.last_data_time = time.time()

        except Exception as e:
            print(f"Parse Error: {e}")
            pass

    def run(self):
        print(f"Connecting to {SERIAL_PORT} for Mouse Control...")
        print("Press Ctrl+C to stop")
        
        try:
            while self.running:
                try:
                    if self.serial_conn is None or not self.serial_conn.is_open:
                        self.serial_conn = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
                        print("Connected! Mouse control active.")
                        self.serial_conn.reset_input_buffer()
                        self.last_data_time = time.time()
                    
                    # 看门狗检查: 如果超过 0.5 秒没有收到有效数据，释放所有按键
                    if time.time() - self.last_data_time > 0.5:
                        self.release_all_keys()
                    
                    if self.serial_conn.in_waiting:
                        # 防积压策略：如果缓冲区数据过多（>200字节，约3帧），直接清空
                        if self.serial_conn.in_waiting > 200:
                            self.serial_conn.reset_input_buffer()
                            # 清空后第一行可能不完整，读出来丢弃
                            self.serial_conn.readline()
                            continue
                        
                        line = self.serial_conn.readline().decode('utf-8').strip()
                        if line:
                            self.parse_and_act(line)
                    
                    # 稍微休眠一下，避免CPU占用过高，但不能太久否则延迟
                    # time.sleep(0.001) 
                    
                except serial.SerialException:
                    print("Serial connection lost. Retrying...")
                    self.release_all_keys()
                    if self.serial_conn:
                        self.serial_conn.close()
                    time.sleep(1)
                except Exception as e:
                    print(f"Unexpected error: {e}")
                    self.release_all_keys()
                    time.sleep(1)
        except KeyboardInterrupt:
            print("\nStopping...")
        finally:
            self.release_all_keys()
            if self.serial_conn:
                self.serial_conn.close()

if __name__ == "__main__":
    controller = MouseController()
    controller.run()
