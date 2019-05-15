import nni
import pandas as pd
import subprocess
import logging
import ctypes
from multiprocessing import Process
import time
import re
import os
import signal
import argparse

proce = []

def read_log():
    log_a = "datalink-A.log"
    log_b = "datalink-B.log"

    f_a = open(log_a).read()
    f_b = open(log_b).read()

    temp = f_a # .decode("gbk")
    findword = r"\Dto log\D \d.\d\d\d\d"
    pattern = re.compile(findword)
    results_a = pattern.findall(temp)

    temp = f_b # .decode("gbk")
    results_b = pattern.findall(temp)

    print(len(results_a))
    print(len(results_b))

    lens = min(len(results_a), len(results_b))
    for i in range(lens - 1):
        print(round((float(results_a[i][9:]) + float(results_b[i][9:])) / 2, 4))
        nni.report_intermediate_result(round((float(results_a[i][9:]) + float(results_b[i][9:])) / 2, 4))

    nni.report_final_result(round((float(results_a[lens - 1][9:]) + float(results_b[lens - 1][9:])) / 2, 4))



def main(params):
    DATA_TIMER_A = params['data_timer_a']
    ACK_TIMER_A = params['ack_timer_a']
    WINDOW_NUMBER_A = params['window_number_a']

    DATA_TIMER_B = params['data_timer_b']
    ACK_TIMER_B = params['ack_timer_b']
    WINDOW_NUMBER_B = params['window_number_b']

    proce.append(subprocess.Popen(['datalink.exe', f'-D {DATA_TIMER_A}', f'-A {ACK_TIMER_A}', f'-W {WINDOW_NUMBER_A}', '-d3', 'A'], preexec_fn=os.setsid))
    proce.append(subprocess.Popen(['datalink.exe', f'-D {DATA_TIMER_B}', f'-A {ACK_TIMER_B}', f'-W {WINDOW_NUMBER_B}', '-d3', 'B'], preexec_fn=os.setsid))

    time.sleep(10)
    os.killpg(proce.pid,signal.SIGUSR1)
    time.sleep(1)

    read_log()



def get_params():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data_timer_a", type=int, default=3100)
    parser.add_argument("--ack_timer_a", type=int, default=1150)
    parser.add_argument("--window_number_a", type=int, default=32)
    parser.add_argument("--data_timer_b", type=int, default=3100)
    parser.add_argument("--ack_timer_b", type=int, default=1150)
    parser.add_argument("--window_number_b", type=int, default=32)

    args, _ = parser.parse_known_args()
    return args


'''if __name__ == "__main__":
    try:
        tuner_params = nni.get_next_parameter()
        logging.debug(tuner_params)
        params = vars(get_params())
        params.update(tuner_params)
        main(params)
    except Exception as ex:
        logging.exception(ex)
        raise
'''

if __name__ == "__main__":
    params = {
        "data_timer_a": 3100,
        "data_timer_b": 3100,
        "ack_timer_a": 1150,
        "ack_timer_b": 1150,
        "window_number_b": 32,
        "window_number_a": 32
    }
    main(params)