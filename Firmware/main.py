from machine import Pin, PWM, ADC
import time
import math

stepper_pins = [Pin(13, Pin.OUT), Pin(12, Pin.OUT), Pin(14, Pin.OUT), Pin(27, Pin.OUT)]
stepper_sequence = [
    [1, 0, 0, 0],
    [1, 1, 0, 0],
    [0, 1, 0, 0],
    [0, 1, 1, 0],
    [0, 0, 1, 0],
    [0, 0, 1, 1],
    [0, 0, 0, 1],
    [1, 0, 0, 1]
]

heater_pwm = PWM(Pin(26), freq=1000, duty_u16=0)

THERMISTOR_PIN = 34
BETA = 3950
R_NOMINAL = 100000
R_SERIES = 100000
T_NOMINAL = 25.0
VS = 3.3

thermistor_adc = ADC(Pin(THERMISTOR_PIN))
thermistor_adc.atten(ADC.ATTN_11DB)

step_delay = 0.005
current_step = 0
motor_running = False

def set_stepper_step(step_index):
    for i in range(4):
        stepper_pins[i].value(stepper_sequence[step_index][i])

def move_stepper(steps, direction_forward=True):
    global current_step
    for _ in range(steps):
        if direction_forward:
            current_step = (current_step + 1) % 8
        else:
            current_step = (current_step - 1) % 8
        set_stepper_step(current_step)
        time.sleep(step_delay)

def set_stepper_speed(speed_rpm):
    global step_delay
    steps_per_rev = 2048
    if speed_rpm > 0:
        step_delay = 60.0 / (steps_per_rev * speed_rpm)
    else:
        step_delay = 0

def read_temperature():
    adc_value = thermistor_adc.read()
    if adc_value == 0 or adc_value >= 4095:
        return None
    voltage = adc_value / 4095.0 * VS
    resistance = R_SERIES * voltage / (VS - voltage)
    steinhart = resistance / R_NOMINAL
    steinhart = math.log(steinhart)
    steinhart /= BETA
    steinhart += 1.0 / (T_NOMINAL + 273.15)
    steinhart = 1.0 / steinhart
    celsius = steinhart - 273.15
    return celsius

def set_heater_power(power_percent):
    duty = int(min(max(power_percent, 0), 100) * 655.35)
    heater_pwm.duty_u16(duty)

def process_command(cmd):
    parts = cmd.strip().split()
    if not parts:
        return
    if parts[0] == "M104" and len(parts) > 1:
        try:
            target_temp = float(parts[1].replace('S', ''))
            print(f"Target temp: {target_temp}")
        except:
            pass
    elif parts[0] == "M106" and len(parts) > 1:
        try:
            speed = int(parts[1].replace('S', ''))
            set_stepper_speed(speed)
            print(f"Speed: {speed}")
        except:
            pass
    elif parts[0] == "M107":
        set_heater_power(0)
        print("Heater off")
    elif parts[0] == "M114":
        temp = read_temperature()
        if temp is not None:
            print(f"Temp: {temp:.1f}C")
    elif parts[0] == "M302":
        print("Moving forward")
        move_stepper(2048, True)
    elif parts[0] == "M303":
        print("Moving backward")
        move_stepper(2048, False)
    elif parts[0] == "M304":
        print("Stepper stopped")
        for pin in stepper_pins:
            pin.value(0)

def main():
    print("ESP32 Hotend Controller Ready")
    print("Commands: M104 Sxxx, M106 Sxxx, M107, M114, M302, M303, M304")
    
    while True:
        temp = read_temperature()
        if temp is not None:
            print(f"Current: {temp:.1f}C", end='\r')
        time.sleep(1)

if __name__ == "__main__":
    main()