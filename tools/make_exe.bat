@echo off
REM Build the doctor-facing SD reader as a standalone Windows exe.
REM Requires: pip install pyinstaller matplotlib openpyxl
python -m PyInstaller --onefile --windowed --name KMM_SD_Reader sd_reader.py
echo.
echo Result: dist\KMM_SD_Reader.exe  (verify: KMM_SD_Reader.exe --selftest)
