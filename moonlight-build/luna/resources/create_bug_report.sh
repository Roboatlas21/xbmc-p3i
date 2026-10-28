#!/bin/bash
cp ../bin/logfile bug_report_file

echo "======= DMESG OUTPUT =======" >> bug_report_file
dmesg | tail -300 >> bug_report_file
echo "============================" >> bug_report_file
tail -300 /storage/.kodi/temp/kodi.log >> bug_report_file
cat bug_report_file | paste