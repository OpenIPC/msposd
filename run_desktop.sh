#!/bin/bash

# Define the command with all the options
CMD="/home/home/src/msposd/msposd --master 127.0.0.1:14550 --baudrate 115200 --osd -r 50 --ahi 3 --matrix 11 -v"

# Execute the command
$CMD
