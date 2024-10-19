#!/bin/sh

cp /etc/wfb.conf /etc/wfb.conf_before_safeboot
cp /etc/majestic.yaml /etc/majestic.yaml_before_safeboot
cp /etc/wfb.conf_safeboot /etc/wfb.conf
cp /etc/majestic.yaml_safeboot /etc/majestic.yaml
reboot