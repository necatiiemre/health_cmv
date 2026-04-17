#!/usr/bin/env python3
"""
DPDK Log to Corporate PDF Converter (MULTIPROCESSING TURBO EDITION)
Parses DPDK test logs and generates a highly formatted corporate PDF.
Strict 5-Page per test layout.
Logic:
1. Calculates Pass/Fail on raw data.
2. Trims tests based on Start/End time difference.
3. Trims TRAILING tests if they lack 'Health Monitor' data (Smart Tail Trimming).
4. Recalculates Duration and Test Finish Time based on the final trimmed list.
"""

import os
import sys
import re
import argparse
import multiprocessing as mp
from datetime import datetime, timedelta
from typing import List, Dict

try:
    from reportlab.lib import colors
    from reportlab.lib.pagesizes import letter
    from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
    from reportlab.lib.units import inch
    from reportlab.platypus import (SimpleDocTemplate, Paragraph, Spacer, 
                                    PageBreak, Image, HRFlowable, Table, TableStyle, Flowable)
    from reportlab.pdfbase.pdfmetrics import stringWidth
    from reportlab.lib.enums import TA_CENTER, TA_LEFT
except ImportError:
    print("ERROR: reportlab is not installed. Please install it with: pip install reportlab")
    sys.exit(1)

# ==========================================
# DEBUG PRINT MODE
# ==========================================
DEBUG_PRINT = False

def debug_print(*args, **kwargs):
    if DEBUG_PRINT:
        print(*args, **kwargs)

# ==========================================
# DEFAULTS
# ==========================================
DEFAULT_LOGO_PATH = "company_logo.png"
DEFAULT_DEVICE_MODEL = "N/A"
DEFAULT_DEVICE_SERIAL = "N/A"
DEFAULT_TESTER_NAME = "N/A"
DEFAULT_QUALITY_CHECKER = "N/A"
DEFAULT_REVISION_DATE = "19/02/2026"
DEFAULT_SOFTWARE_START_TIME = "N/A"
DEFAULT_UNIT_POWER_ON_TIME = "N/A"
DEFAULT_TEST_START_TIME = "N/A"
DEFAULT_TEST_FINISH_TIME = "N/A"
DEFAULT_POWER_OFF_TIME = "N/A"
DEFAULT_SOFTWARE_END_TIME = "N/A"

RE_HEALTH_BLOCK = re.compile(r'\[HEALTH\]\s+(\d+)\s+\|\s+\d+\s+\|\s+\d+\s+\|\s+(\d+)\s+\|\s+(\d+)\s+\|\s+(\d+)\s+\|\s+(\d+)\s+\|\s+(\d+)\s+\|')
RE_DURATION = re.compile(r'(\d+)\s*(sn|sec)')
RE_MAIN_TABLE = re.compile(r'^\d+\s*║')
RE_RAW_TABLE = re.compile(r'^P\d+\s*║')

# ==========================================
# TIME UTILS
# ==========================================
def parse_flexible_time(time_str: str):
    """Parses time string with multiple possible formats."""
    if not time_str or time_str == "N/A":
        return None
    
    # Clean up the string
    time_str = time_str.strip()
    
    # List of supported formats
    formats = [
        "%B %d, %Y %H:%M:%S",  # February 22, 2026 01:47:15
        "%d/%m/%Y %H:%M:%S",   # 22/02/2026 01:47:15
        "%Y-%m-%d %H:%M:%S",   # 2026-02-22 01:47:15
        "%H:%M:%S"             # 01:47:15 (Just time)
    ]
    
    for fmt in formats:
        try:
            dt = datetime.strptime(time_str, fmt)
            # If only time is provided, attach today's date for calculation purposes
            if fmt == "%H:%M:%S":
                now = datetime.now()
                dt = dt.replace(year=now.year, month=now.month, day=now.day)
            return dt
        except ValueError:
            continue
    return None

def format_duration(duration_str: str) -> str:
    try:
        match = re.search(r'\d+', duration_str)
        if match:
            seconds = int(match.group())
            h = seconds // 3600
            m = (seconds % 3600) // 60
            s = seconds % 60
            return f"{seconds}s ({h:02d}:{m:02d}:{s:02d})"
    except Exception:
        pass
    return duration_str

# ==========================================
# TEXT SHRINKER (CACHED)
# ==========================================
STRING_WIDTH_CACHE = {}

class ShrinkToFit(Flowable):
    def __init__(self, text, font_name='Helvetica', font_size=7, text_color=colors.black):
        Flowable.__init__(self)
        self.text = str(text)
        self.font_name = font_name
        self.font_size = font_size
        self.text_color = text_color
        
    def wrap(self, availWidth, availHeight):
        self.availWidth = availWidth
        cache_key = (self.text, self.font_name, self.font_size)
        if cache_key not in STRING_WIDTH_CACHE:
            STRING_WIDTH_CACHE[cache_key] = stringWidth(self.text, self.font_name, self.font_size)
            
        width = STRING_WIDTH_CACHE[cache_key]
        self.scale = 1.0
        if width > availWidth and width > 0:
            self.scale = (availWidth - 2) / width
        
        self.draw_font_size = self.font_size * self.scale
        self.width = availWidth
        self.height = self.font_size * 1.0 
        return self.width, self.height

    def draw(self):
        self.canv.saveState()
        self.canv.setFillColor(self.text_color)
        self.canv.setFont(self.font_name, self.draw_font_size)
        self.canv.drawCentredString(self.width / 2.0, 1, self.text)
        self.canv.restoreState()


# ==========================================
# 1. PARSER
# ==========================================
class LogParser:
    def __init__(self, filepath: str):
        self.filepath = filepath
        self.data = {
            "metadata": {},
            "phases": [],
            "test_duration": "N/A",
            "first_health": {},
            "last_health": {},
            "mismatches": [],
            "reference_phase_name": "N/A",
            "fw_mismatch": None,
            "power_status_mismatch": None,
            "fw_check_result": None,         # "PASSED" or "FAILED" with details
            "power_status_check_result": None # "PASSED" or "FAILED" with details
        }

    def parse(self):
        with open(self.filepath, 'r', encoding='utf-8', errors='ignore') as f:
            state = "NORMAL"
            current_phase = None
            health_capture_state = None
            health_table_state = None
            
            meta_keys = ["Software Start Time", "Unit Power On Time", "Test Start Time", "Test Finish Time",
                        "Power Off Time", "Software End Time",
                        "Test Name", "ATE Serial Number", "Bilgem Number", "Serial Number", "Tester Name", "Quality Checker", "Unit Name", "Revision Date", "Revision"]

            for line in f:
                line_clean = line.strip()
                if not line_clean:
                    continue

                is_meta_line = False
                for mk in meta_keys:
                    if line_clean.startswith(mk):
                        parts = line_clean.split(":", 1)
                        if len(parts) == 2:
                            self.data["metadata"][parts[0].strip()] = parts[1].strip()
                            is_meta_line = True
                            break
                if is_meta_line: continue

                # --- HEALTH BLOCK & PDF DATA CAPTURE ---
                if "[HEALTH]" in line_clean:
                    h_line = line_clean.replace("[HEALTH]", "").strip()

                    # Capture FW version check results
                    if "ERROR: SW firmware version mismatch" in h_line:
                        self.data["fw_mismatch"] = {"assistant": "N/A", "manager": "N/A"}
                        self.data["fw_check_result"] = {"status": "FAILED", "detail": ""}
                    elif "FW version check PASSED" in h_line:
                        # e.g. "FW version check PASSED: Assistant=1.2.3 Manager=1.2.3"
                        detail = h_line.split("PASSED:")[-1].strip() if "PASSED:" in h_line else ""
                        self.data["fw_check_result"] = {"status": "PASSED", "detail": detail}
                    elif self.data["fw_mismatch"] is not None:
                        if "Assistant FPGA SW_FW" in h_line:
                            self.data["fw_mismatch"]["assistant"] = h_line.split("=")[-1].strip()
                            self.data["fw_check_result"]["detail"] = f"Assistant={self.data['fw_mismatch']['assistant']}"
                        elif "Manager" in h_line and "FPGA SW_FW" in h_line:
                            self.data["fw_mismatch"]["manager"] = h_line.split("=")[-1].strip()
                            self.data["fw_check_result"]["detail"] += f" Manager={self.data['fw_mismatch']['manager']}"

                    # Capture 28V power status check results
                    if "ERROR: 28V Power Status MISMATCH" in h_line:
                        self.data["power_status_mismatch"] = {"expected": "N/A", "actual": "N/A"}
                        self.data["power_status_check_result"] = {"status": "FAILED", "expected": "N/A", "actual": "N/A"}
                    elif "28V Power Status Check PASSED" in h_line:
                        self.data["power_status_check_result"] = {"status": "PASSED", "expected": "", "actual": ""}

                    # Capture Expected/Actual lines for 28V (both PASSED and FAILED cases)
                    if self.data["power_status_check_result"] is not None:
                        if "Expected:" in h_line and "Primary=" in h_line:
                            exp_val = re.sub(r'\s*\(0x[0-9A-Fa-f]+\)', '', h_line.split("Expected:")[-1].strip())
                            self.data["power_status_check_result"]["expected"] = exp_val
                            if self.data["power_status_mismatch"] is not None:
                                self.data["power_status_mismatch"]["expected"] = exp_val
                        elif "Actual:" in h_line and "Primary=" in h_line:
                            act_val = re.sub(r'\s*\(0x[0-9A-Fa-f]+\)', '', h_line.split("Actual:")[-1].strip())
                            self.data["power_status_check_result"]["actual"] = act_val
                            if self.data["power_status_mismatch"] is not None:
                                self.data["power_status_mismatch"]["actual"] = act_val

                    if h_line.startswith("============ ASSISTANT FPGA"):
                        health_capture_state = "ASSISTANT"
                        health_table_state = "AST_META"
                    elif h_line.startswith("---- ASSISTANT FPGA Port Status"):
                        health_table_state = "AST_TABLE"
                    elif h_line.startswith("============ MANAGER FPGA"):
                        health_capture_state = "MANAGER"
                        health_table_state = "MGR_META"
                    elif h_line.startswith("---- MANAGER FPGA Port Status"):
                        health_table_state = "MGR_TABLE"
                    elif h_line.startswith("============ MCU - Status"):
                        health_capture_state = "MCU"
                        health_table_state = "MCU_META_GEN"
                    elif h_line.startswith("---- Current Data"):
                        health_table_state = "MCU_CURR"
                    elif h_line.startswith("---- Voltage Data"):
                        health_table_state = "MCU_VOLT"
                    elif h_line.startswith("---- Temperatures"):
                        health_table_state = "MCU_TEMP"
                    elif h_line.startswith("================================================"):
                        health_capture_state = None
                        health_table_state = None
                    else:
                        if current_phase is not None:
                            if health_capture_state == "ASSISTANT":
                                if health_table_state == "AST_META":
                                    current_phase["ast_meta"].append(h_line)
                                elif health_table_state == "AST_TABLE" and not h_line.startswith("Port |") and not h_line.startswith("-----|"):
                                    parts = [p.strip() for p in h_line.split('|') if p.strip()]
                                    if len(parts) >= 8: current_phase["ast_table"].append(parts)
                            elif health_capture_state == "MANAGER":
                                if health_table_state == "MGR_META":
                                    current_phase["mgr_meta"].append(h_line)
                                elif health_table_state == "MGR_TABLE" and not h_line.startswith("Port |") and not h_line.startswith("-----|"):
                                    parts = [p.strip() for p in h_line.split('|') if p.strip()]
                                    if len(parts) >= 8: current_phase["mgr_table"].append(parts)
                            elif health_capture_state == "MCU":
                                if health_table_state == "MCU_META_GEN":
                                    if "DevID=" in h_line:
                                        current_phase["mcu_meta_general"].append(h_line)
                                    elif "SUCCESS" in h_line or "FAIL" in h_line or "PBIT=" in h_line:
                                        current_phase["mcu_meta_status"].append(h_line)
                                elif health_table_state == "MCU_CURR":
                                    if "|" in h_line and "Curr" not in h_line and "---" not in h_line:
                                        parts = [p.strip() for p in h_line.split('|') if p.strip()]
                                        if len(parts) >= 2: current_phase["mcu_curr_table"].append(parts)
                                elif health_table_state == "MCU_VOLT":
                                    if "|" in h_line and "Volt" not in h_line and "---" not in h_line:
                                        parts = [p.strip() for p in h_line.split('|') if p.strip()]
                                        if len(parts) >= 2: current_phase["mcu_volt_table"].append(parts)
                                elif health_table_state == "MCU_TEMP":
                                    if "=" in h_line:
                                        current_phase["mcu_meta_temp"].append(h_line)

                    match = RE_HEALTH_BLOCK.search(line_clean)
                    if match:
                        port = int(match.group(1))
                        drops = sum(int(match.group(i)) for i in range(2, 7))
                        if port not in self.data["first_health"]:
                            self.data["first_health"][port] = drops
                        self.data["last_health"][port] = drops
                        if current_phase is not None:
                            current_phase["has_health"] = True
                    continue

                if line_clean.startswith("========== [TEST"):
                    if current_phase: self.data["phases"].append(current_phase)
                    
                    match = RE_DURATION.search(line_clean)
                    if match: self.data["test_duration"] = match.group(1) + "s"

                    current_phase = {
                        "name": line_clean.strip("= []").replace("sn", "sec"),
                        "has_health": False, 
                        "main_table": [],
                        "raw_multi_table": [],
                        "port12_table": [],
                        "port13_table": [],
                        "ptp_table": [],
                        "ast_meta": [],
                        "ast_table": [],
                        "mgr_meta": [],
                        "mgr_table": [],
                        "mcu_meta_general": [],
                        "mcu_meta_status": [],
                        "mcu_curr_table": [],
                        "mcu_volt_table": [],
                        "mcu_meta_temp": []
                    }
                    state = "PHASE_MAIN_TABLE"
                    continue
                
                if line_clean.startswith("========== [WARM-UP"):
                    if current_phase: self.data["phases"].append(current_phase)
                    current_phase = None
                    state = "NORMAL"
                    continue

                # --- PTP TABLOSU YAKALAMA ---
                if line_clean == "--- PTP Statistics ---":
                    if current_phase is not None:
                        state = "PHASE_PTP_TABLE"
                    continue

                if current_phase and state == "PHASE_PTP_TABLE":
                    if line_clean.startswith("Port") or line_clean.startswith("---"):
                        continue 
                    elif line_clean.startswith("Q5") or line_clean.startswith("PTP Debug"):
                        state = "NORMAL" 
                        continue
                    else:
                        parts = line_clean.split()
                        if len(parts) >= 11:
                            ptp_row = [parts[0], parts[2], parts[3]] + parts[4:11]
                            current_phase["ptp_table"].append(ptp_row)
                        continue

                # --- DİĞER TABLOLARI OKUMA ---
                if current_phase and state.startswith("PHASE_"):
                    clean_table_line = line_clean.replace("│", "║").strip("║ ")
                    if "║" in clean_table_line:
                        if RE_MAIN_TABLE.match(clean_table_line):
                            parts = [p.strip() for p in clean_table_line.split("║")]
                            if len(parts) >= 12: current_phase["main_table"].append(parts)
                        elif RE_RAW_TABLE.match(clean_table_line):
                            parts = [p.strip() for p in clean_table_line.split("║")]
                            if len(parts) >= 11: current_phase["raw_multi_table"].append(parts)
                        elif state == "PHASE_PORT12_TABLE" and not "RX Pkts" in line_clean:
                            parts = [p.strip() for p in clean_table_line.split("║")]
                            if len(parts) >= 6: current_phase["port12_table"].append(parts)
                            state = "PHASE_MAIN_TABLE"
                        elif state == "PHASE_PORT13_TABLE" and not "RX Pkts" in line_clean:
                            parts = [p.strip() for p in clean_table_line.split("║")]
                            if len(parts) >= 6: current_phase["port13_table"].append(parts)
                            state = "PHASE_MAIN_TABLE"
                    else:
                        if "Port 12 RX" in line_clean: state = "PHASE_PORT12_TABLE"
                        elif "Port 13 RX" in line_clean: state = "PHASE_PORT13_TABLE"

            if current_phase:
                self.data["phases"].append(current_phase)

        # 1. EVALUATE RESULTS (Pass/Fail tüm veriler üzerinden değişmeden hesaplanır)
        self._evaluate_test_results()
        
        all_phases = self.data["phases"]
        valid_phases = []
        
        # --- AŞAMA 1: ZAMAN FARKINA GÖRE TEST SAYISINI LİMİTLEME ---
        start_time_str = self.data["metadata"].get("Test Start Time", "")
        finish_time_str = self.data["metadata"].get("Test Finish Time", "")
        
        start_dt = parse_flexible_time(start_time_str)
        finish_dt = parse_flexible_time(finish_time_str)
        
        delta_sec = 0
        if start_dt and finish_dt:
            delta_sec = int((finish_dt - start_dt).total_seconds())
            if delta_sec < 0: delta_sec += 86400 # Gece yarısı
        
        # Isınma testini atıyoruz
        if len(all_phases) > 0:
            valid_phases = all_phases[1:]
        else:
            valid_phases = []
            
        # Zaman farkına göre fazlalıkları kes
        if delta_sec > 0 and len(valid_phases) > delta_sec:
            valid_phases = valid_phases[:delta_sec]
            
        # --- AŞAMA 2: KUYRUKTAN VERİSİZ FAZLARI SİLME (Smart Tail Trimming) ---
        # Listenin sonundan geriye doğru git, eğer ne health datası ne de port istatistiği yoksa sil.
        # Health datası VEYA port istatistiği olan birini bulduğunda dur.
        while valid_phases and not valid_phases[-1].get("has_health") and not valid_phases[-1].get("main_table"):
            valid_phases.pop()
            
        # --- AŞAMA 3: İSİMLERİ ve METADATA SÜRELERİNİ GÜNCELLEME ---
        final_count = len(valid_phases)
        
        # Faz isimlerini yeniden numaralandır (TEST 1 sn, TEST 2 sn...)
        for i, phase in enumerate(valid_phases):
            phase["name"] = f"TEST {i+1} sec"
            
        self.data["phases"] = valid_phases
        self.data["test_duration"] = f"{final_count}s"
        
        # Test Finish Time'ı yeni süreye göre güncelle
        if start_dt:
            new_finish_dt = start_dt + timedelta(seconds=final_count)
            # Tarih formatını orijinal formata sadık kalarak güncelle (Örn: February 22, 2026 01:50:00)
            if "February" in start_time_str or "January" in start_time_str: # Kabaca bir kontrol
                self.data["metadata"]["Test Finish Time"] = new_finish_dt.strftime("%B %d, %Y %H:%M:%S")
            else:
                self.data["metadata"]["Test Finish Time"] = new_finish_dt.strftime("%d/%m/%Y %H:%M:%S")
        
        return self.data

    def _evaluate_test_results(self):
        global_fail = False
        mismatches = []
        target_phase = None
        
        for phase in reversed(self.data["phases"]):
            if phase.get("main_table") and phase.get("has_health"):
                target_phase = phase
                break

        if target_phase:
            self.data["reference_phase_name"] = target_phase["name"]
            for row in target_phase["main_table"]:
                if len(row) > 9 and row[0].isdigit():
                    try:
                        port = int(row[0])
                        table_lost = int(row[9].replace(',', '').strip())
                        baseline = self.data["first_health"].get(port, 0)
                        final = self.data["last_health"].get(port, 0)
                        real_drops = final - baseline
                        
                        if real_drops != table_lost:
                            global_fail = True
                            mismatches.append(f"Port {port} Mismatch -> Table: {table_lost} | Device: {real_drops}")
                    except ValueError:
                        pass

        # FW version mismatch also causes test failure
        if self.data.get("fw_mismatch") is not None:
            global_fail = True
            mismatches.append(
                f"SW FW Mismatch -> Assistant: {self.data['fw_mismatch']['assistant']} | "
                f"Manager: {self.data['fw_mismatch']['manager']}"
            )
            self.data["metadata"]["FW Mismatch"] = (
                f"Assistant: {self.data['fw_mismatch']['assistant']} | "
                f"Manager: {self.data['fw_mismatch']['manager']}"
            )

        # MCU data missing in any phase causes test failure
        mcu_missing_phases = []
        for phase in self.data["phases"]:
            if phase.get("main_table"):  # sadece geçerli test fazlarını kontrol et
                has_mcu = (phase.get("mcu_curr_table") or phase.get("mcu_volt_table") or phase.get("mcu_meta_temp"))
                if not has_mcu:
                    mcu_missing_phases.append(phase.get("name", "?"))

        if mcu_missing_phases:
            global_fail = True
            total_missing = len(mcu_missing_phases)
            if total_missing <= 5:
                phase_list = ", ".join(mcu_missing_phases)
            else:
                phase_list = ", ".join(mcu_missing_phases[:5]) + f" ... (+{total_missing - 5} more)"
            mismatches.append(f"MCU Data Missing in {total_missing} phase(s): {phase_list}")

        # 28V power status mismatch also causes test failure
        if self.data.get("power_status_mismatch") is not None:
            global_fail = True
            mismatches.append(
                f"28V Power Status Mismatch -> "
                f"Expected: {self.data['power_status_mismatch']['expected']} | "
                f"Actual: {self.data['power_status_mismatch']['actual']}"
            )
            self.data["metadata"]["28V Mismatch"] = (
                f"Expected: {self.data['power_status_mismatch']['expected']} | "
                f"Actual: {self.data['power_status_mismatch']['actual']}"
            )

        self.data["metadata"]["Test Result"] = "Fail" if global_fail else "Pass"
        self.data["mismatches"] = mismatches

        # Pass validation check results to metadata for PDF rendering
        if self.data.get("fw_check_result"):
            self.data["metadata"]["_fw_check_result"] = self.data["fw_check_result"]
        if self.data.get("power_status_check_result"):
            self.data["metadata"]["_power_status_check_result"] = self.data["power_status_check_result"]


# ==========================================
# 2. PDF TEMPLATE AND GENERATION
# ==========================================
class PDFReportTemplate:
    def __init__(self, logo_path: str = None):
        self.logo_path = logo_path
        self.styles = getSampleStyleSheet()
        self._setup_custom_styles()

        self.document_name = "TEST REPORT"
        self.document_number = "IPPP-HW-#####"
        self.test_result = "Pass"
        self.revision = "0.0A"
        self.revision_date = DEFAULT_REVISION_DATE
        self.report_date = datetime.now().strftime('%d.%m.%Y')
        self.software_start_time = DEFAULT_SOFTWARE_START_TIME
        self.unit_power_on_time = DEFAULT_UNIT_POWER_ON_TIME
        self.test_start_time = DEFAULT_TEST_START_TIME
        self.test_finish_time = DEFAULT_TEST_FINISH_TIME
        self.power_off_time = DEFAULT_POWER_OFF_TIME
        self.software_end_time = DEFAULT_SOFTWARE_END_TIME
        self.tester_name = DEFAULT_TESTER_NAME
        self.quality_checker_name = DEFAULT_QUALITY_CHECKER
        self.device_model = DEFAULT_DEVICE_MODEL
        self.ate_serial_number = "N/A"
        self.bilgem_number = "N/A"
        self.device_serial = DEFAULT_DEVICE_SERIAL
        self.chunk_info = ""
        self.fw_mismatch_info = None

    def apply_metadata(self, meta: Dict[str, str]):
        if "Test Name" in meta: self.document_name = meta["Test Name"]
        if "ATE Serial Number" in meta: self.ate_serial_number = meta["ATE Serial Number"]
        if "Bilgem Number" in meta: self.bilgem_number = meta["Bilgem Number"]
        if "Serial Number" in meta: self.device_serial = meta["Serial Number"]
        if "Tester Name" in meta: self.tester_name = meta["Tester Name"]
        if "Quality Checker" in meta: self.quality_checker_name = meta["Quality Checker"]
        if "Unit Name" in meta: self.device_model = meta["Unit Name"]
        if "Software Start Time" in meta: self.software_start_time = meta["Software Start Time"]
        if "Unit Power On Time" in meta: self.unit_power_on_time = meta["Unit Power On Time"]
        if "Test Start Time" in meta: self.test_start_time = meta["Test Start Time"]
        if "Test Finish Time" in meta: self.test_finish_time = meta["Test Finish Time"]
        if "Power Off Time" in meta: self.power_off_time = meta["Power Off Time"]
        if "Software End Time" in meta: self.software_end_time = meta["Software End Time"]
        if "Revision Date" in meta: self.revision_date = meta["Revision Date"]
        if "Revision" in meta: self.revision = meta["Revision"]
        if "Test Result" in meta: self.test_result = meta["Test Result"]
        self.fw_mismatch_info = meta.get("FW Mismatch", None)

    def _setup_custom_styles(self):
        self.styles.add(ParagraphStyle(name='CustomTitle', parent=self.styles['Heading1'], fontSize=24,
            textColor=colors.HexColor('#1a5490'), spaceAfter=30, alignment=TA_CENTER, fontName='Helvetica-Bold', leading=28))
        self.styles.add(ParagraphStyle(name='SectionHeader', parent=self.styles['Heading2'], fontSize=16,
            textColor=colors.HexColor('#1a5490'), spaceAfter=20, spaceBefore=24, fontName='Helvetica-Bold', leading=20))
        self.styles.add(ParagraphStyle(name='EnhancedBody', parent=self.styles['Normal'], fontSize=10, leading=14,
            spaceAfter=8, alignment=TA_LEFT, fontName='Helvetica'))
        self.styles.add(ParagraphStyle(name='PhaseTitle', fontSize=12, textColor=colors.white, backColor=colors.HexColor('#e67e22'), 
            spaceAfter=10, spaceBefore=15, fontName='Helvetica-Bold', alignment=TA_CENTER, borderPadding=5))
        self.styles.add(ParagraphStyle(name='SubTitle', fontSize=10, fontName='Helvetica-Bold', spaceAfter=6, spaceBefore=10))

    def _create_header_footer(self, canvas_obj, doc, is_cover_page=False):
        canvas_obj.saveState()
        page_width, page_height = letter

        if self.logo_path and os.path.exists(self.logo_path):
            try:
                header_logo_size = 50
                canvas_obj.drawImage(self.logo_path, 0.5 * inch, page_height - 1.07 * inch,
                    width=header_logo_size, height=header_logo_size, preserveAspectRatio=True, mask='auto')
            except Exception: pass

        if not is_cover_page:
            canvas_obj.setFont('Helvetica-Bold', 10)
            canvas_obj.setFillColor(colors.HexColor('#DD0000'))
            canvas_obj.drawCentredString(page_width / 2, page_height - 0.65 * inch, "RESTRICTED")

        canvas_obj.setFont('Helvetica-Bold', 10)
        canvas_obj.setFillColor(colors.HexColor('#2c5aa0'))
        canvas_obj.drawCentredString(page_width / 2, page_height - 0.85 * inch, "INTEGRATED PROCESSING POOL PLATFORM PROJECT")

        if doc.page > 1:
            canvas_obj.setFont('Helvetica-Bold', 8)
            canvas_obj.setFillColor(colors.HexColor('#555555'))
            full_header_title = f"{self.device_model} {self.document_name} TEST RESULT"
            canvas_obj.drawCentredString(page_width / 2, page_height - 0.98 * inch, full_header_title)

        canvas_obj.setStrokeColor(colors.HexColor('#2c5aa0'))
        canvas_obj.setLineWidth(1.5)
        canvas_obj.line(0.5 * inch, page_height - 1.10 * inch, page_width - 0.5 * inch, page_height - 1.10 * inch)

        canvas_obj.setFillColor(colors.HexColor('#f0f0f0'))
        canvas_obj.rect(0.5 * inch, 0.85 * inch, page_width - 1.0 * inch, 0.25 * inch, fill=1, stroke=0)
        canvas_obj.setStrokeColor(colors.HexColor('#2c5aa0'))
        canvas_obj.setLineWidth(1)
        canvas_obj.rect(0.5 * inch, 0.85 * inch, page_width - 1.0 * inch, 0.25 * inch, fill=0, stroke=1)

        canvas_obj.setFont('Helvetica', 8)
        canvas_obj.setFillColor(colors.HexColor('#1a1a1a'))
        canvas_obj.drawString(0.6 * inch, 0.93 * inch, self.document_number)

        separator1_x = 0.5 * inch + (page_width - 1.0 * inch) / 3
        separator2_x = 0.5 * inch + 2 * (page_width - 1.0 * inch) / 3
        canvas_obj.line(separator1_x, 0.85 * inch, separator1_x, 1.10 * inch)
        canvas_obj.line(separator2_x, 0.85 * inch, separator2_x, 1.10 * inch)

        canvas_obj.drawCentredString((separator1_x + separator2_x) / 2, 0.93 * inch, f"Revision Date: {self.revision_date}")
        
        page_text = f"Report Date: {self.report_date} {self.chunk_info}" if is_cover_page else f"Report Date: {self.report_date} | Page {doc.page} {self.chunk_info}"
        canvas_obj.drawCentredString((separator2_x + (page_width - 0.5 * inch)) / 2, 0.93 * inch, page_text)

        canvas_obj.setFont('Helvetica-Bold', 10)
        canvas_obj.setFillColor(colors.HexColor('#DD0000'))
        canvas_obj.drawCentredString(page_width / 2, 0.55 * inch, "RESTRICTED")

        if doc.page > 1:
            canvas_obj.setFont('Helvetica', 6)
            canvas_obj.setFillColor(colors.HexColor('#666666'))
            x_pos = 0.25 * inch
            canvas_obj.saveState()
            canvas_obj.translate(x_pos, page_height - 3.37 * inch)
            canvas_obj.rotate(90)
            canvas_obj.drawString(0, 0, "The contents of this document are the property of TUBITAK BILGEM")
            canvas_obj.drawString(0, -0.10 * inch, "and should not be reproduced, copied or disclosed to a third party")
            canvas_obj.drawString(0, -0.20 * inch, "without the written consent of the proprietor.")
            canvas_obj.restoreState()

            canvas_obj.saveState()
            canvas_obj.translate(x_pos, page_height / 2 - 1.07 * inch)
            canvas_obj.rotate(90)
            canvas_obj.drawString(0, 0, "© TUBITAK BILGEM - Informatics and Information Security Advanced Technologies Research Center")
            canvas_obj.drawString(0, -0.10 * inch, "P.O. Box 74, Gebze, 41470 Kocaeli, Turkiye")
            canvas_obj.drawString(0, -0.20 * inch, "Tel: (0262) 675 30 00, Fax: (0262) 648 11 00")
            canvas_obj.restoreState()

            canvas_obj.saveState()
            canvas_obj.translate(x_pos, 1.23 * inch)
            canvas_obj.rotate(90)
            canvas_obj.drawString(0, 0, "The contents of this document are the property of TUBITAK BILGEM.")
            canvas_obj.drawString(0, -0.10 * inch, "It may not be reproduced, copied or disclosed to third parties")
            canvas_obj.drawString(0, -0.20 * inch, "without the written consent of the proprietor.")
            canvas_obj.restoreState()

        canvas_obj.restoreState()

    def _draw_cover_page_border(self, canvas_obj, doc):
        canvas_obj.saveState()
        border_color = colors.HexColor('#2c5aa0')
        margin = 0.40 * inch 
        canvas_obj.setStrokeColor(border_color)
        canvas_obj.setLineWidth(4)
        canvas_obj.setLineCap(1)
        canvas_obj.setLineJoin(1)
        canvas_obj.rect(margin, margin, letter[0] - 2 * margin, letter[1] - 2 * margin, stroke=1, fill=0)
        canvas_obj.restoreState()

    def _create_cover_page(self) -> List:
        elements = [Spacer(1, 0.3 * inch)]

        if self.logo_path and os.path.exists(self.logo_path):
            try:
                cover_logo_size = 160 
                logo_img = Image(self.logo_path, width=cover_logo_size, height=cover_logo_size, kind='proportional')
                logo_img.hAlign = 'CENTER'
                elements.append(logo_img)
            except Exception: pass

        elements.append(Spacer(1, 0.2 * inch))
        elements.append(HRFlowable(width="80%", thickness=2, color=colors.HexColor('#2c5aa0'), spaceAfter=0.25*inch, hAlign='CENTER'))
        elements.append(Paragraph('<para alignment="center" fontSize="14" textColor="#2c5aa0"><b>INFORMATICS AND INFORMATION SECURITY RESEARCH CENTER</b></para>', self.styles['Normal']))
        elements.append(Spacer(1, 0.35 * inch))
        elements.append(Paragraph('<para alignment="center" fontSize="12" textColor="#1a1a1a"><b>INTEGRATED PROCESSING POOL PLATFORM PROJECT</b></para>', self.styles['Normal']))
        elements.append(Spacer(1, 0.35 * inch))
        
        title_text = f"""
        <para alignment="center" fontSize="16" textColor="#1a1a1a" leading="28">
        <b>{self.device_model}</b><br/>
        <font size="14"><b>{self.document_name}</b></font><br/>
        <font size="14"><b>TEST RESULT</b></font>
        </para>
        """
        elements.append(Paragraph(title_text, self.styles['Normal']))
        elements.append(Spacer(1, 0.3 * inch))

        status_color = "#00AA00" if self.test_result.upper() == "PASS" else "#DD0000"

        fw_mismatch_line = ""
        if self.fw_mismatch_info:
            fw_mismatch_line = f"<br/><font color='#DD0000'><b>SW FW MISMATCH: {self.fw_mismatch_info}</b></font>"

        doc_info = f"""
        <para alignment="center" fontSize="10" textColor="#1a1a1a">
        <b>Document Number:</b> {self.document_number}<br/>
        <b>Test Result:</b> <font color='{status_color}'><b>{self.test_result}</b></font>{fw_mismatch_line}<br/>
        <b>Revision:</b> {self.revision}<br/>
        <b>Revision Date:</b> {self.revision_date}<br/>
        <b>Report Date:</b> {self.report_date}<br/>
        <b>Tester:</b> {self.tester_name}<br/>
        <b>Quality Checker:</b> {self.quality_checker_name}
        </para>
        """
        elements.append(Paragraph(doc_info, self.styles['Normal']))
        elements.append(Spacer(1, 0.35 * inch))

        elements.append(HRFlowable(width="80%", thickness=2, color=colors.HexColor('#2c5aa0'), spaceAfter=0.15*inch, hAlign='CENTER'))
        
        footer_text = """
        <para alignment="center" fontSize="8" textColor="#1a1a1a">
        <b>@TUBITAK BILGEM</b><br/>
        Informatics and Information Security Research Center<br/>
        P.C.74, 41470 GEBZE, KOCAELI, TURKIYE<br/>
        Tel : +90 (262) 675 30 00,  Fax: +90 (262) 648 11 00<br/>
        www.bilgem.tubitak.gov.tr | bilgem@tubitak.gov.tr
        </para>
        """
        elements.append(Paragraph(footer_text, self.styles['Normal']))
        elements.append(PageBreak())
        return elements

    def _create_product_info_section(self, test_duration: str) -> List:
        elements = []
        elements.append(Paragraph("Product Information", self.styles['SectionHeader']))

        table_data = [
            ["Test Duration:", test_duration],
            ["Device Model:", self.device_model],
            ["ATE Serial Number:", self.ate_serial_number],
            ["Bilgem Number:", self.bilgem_number],
            ["Device Serial:", self.device_serial],
            ["Tester:", self.tester_name],
            ["Quality Checker:", self.quality_checker_name],
            ["Software Start Time:", self.software_start_time],
            ["Unit Power On Time:", self.unit_power_on_time],
            ["Test Start Time:", self.test_start_time],
            ["Test Finish Time:", self.test_finish_time],
            ["Power Off Time:", self.power_off_time],
            ["Software End Time:", self.software_end_time]
        ]
        
        t = Table(table_data, colWidths=[2.0*inch, 4.5*inch])
        t.setStyle(TableStyle([
            ('TEXTCOLOR', (0, 0), (-1, -1), colors.HexColor('#1a1a1a')),
            ('ALIGN', (0, 0), (-1, -1), 'LEFT'),
            ('VALIGN', (0, 0), (-1, -1), 'MIDDLE'),
            ('FONTNAME', (0, 0), (0, -1), 'Helvetica-Bold'),
            ('FONTNAME', (1, 0), (1, -1), 'Helvetica'),
            ('FONTSIZE', (0, 0), (-1, -1), 11),
            ('BOTTOMPADDING', (0, 0), (-1, -1), 12),
            ('TOPPADDING', (0, 0), (-1, -1), 12),
            ('LINEBELOW', (0, 0), (-1, -1), 0.5, colors.HexColor('#cccccc')),
        ]))
        
        elements.append(t)
        elements.append(Spacer(1, 20))
        note_text = (
            '<b>Note:</b> Data testing begins approximately 240 seconds after '
            'the power source is turned on. The first 120 seconds are allocated '
            'for system initialization, followed by an additional 120 seconds of '
            'warm-up phase before the actual test data collection starts.'
        )
        note_style = ParagraphStyle(
            'ProductInfoNote',
            parent=self.styles['Normal'],
            fontSize=9,
            textColor=colors.HexColor('#555555'),
            leading=14,
            spaceBefore=6,
            spaceAfter=6,
        )
        elements.append(Paragraph(note_text, note_style))
        elements.append(PageBreak())
        return elements

    def _create_key_val_table(self, meta_lines: List[str]):
        full_text = " ".join(meta_lines)
        pairs = re.findall(r'([^|=]+)=([^|]+)', full_text)
        if not pairs: return None
            
        headers = []
        values = []
        status_en_map = {"0x00": "Disabled", "0x01": "Enabled for Manager Switch", "0x03": "Enabled for Assistant FPGA", "0x05": "Enabled for MCU"}
        
        for k, v in pairs:
            k = k.strip()
            v = v.strip()
            
            if k == "DevID":
                k = "Device ID"
                try:
                    cleaned_val = v.rstrip('0')
                    if cleaned_val == '0x': cleaned_val = '0x0'
                    dec_val = int(cleaned_val, 16) if cleaned_val.startswith('0x') else int(cleaned_val)
                    v = f"{v} ({dec_val})"
                except ValueError:
                    pass
            elif k == "StatusEnable":
                k = "Status Enable"
                if v in status_en_map: v = status_en_map[v]
            elif k == "FO Trans1":
                k = "FO Transceiver"
            
            if v.endswith('C') and any(char.isdigit() for char in v):
                v = f"{v[:-1]} °C"
                
            if v.upper() == "SUCCESS":
                v_flow = ShrinkToFit(v, font_name='Helvetica-Bold', font_size=8, text_color=colors.HexColor('#00AA00'))
            elif v.upper() in ["FAIL", "ERROR"]:
                v_flow = ShrinkToFit(v, font_name='Helvetica-Bold', font_size=8, text_color=colors.HexColor('#DD0000'))
            else:
                v_flow = ShrinkToFit(v, font_name='Helvetica-Bold', font_size=8, text_color=colors.HexColor('#222222'))
                
            headers.append(ShrinkToFit(k, font_name='Helvetica-Bold', font_size=8, text_color=colors.whitesmoke))
            values.append(v_flow)
            
        col_width = (7.0 * inch) / len(headers)
        t = Table([headers, values], colWidths=[col_width]*len(headers))
        t.setStyle(TableStyle([
            ('BACKGROUND', (0, 0), (-1, 0), colors.HexColor('#2c5aa0')),
            ('ALIGN', (0, 0), (-1, -1), 'CENTER'),
            ('VALIGN', (0, 0), (-1, -1), 'MIDDLE'),
            ('BOTTOMPADDING', (0, 0), (-1, -1), 5),
            ('TOPPADDING', (0, 0), (-1, -1), 5),
            ('GRID', (0, 0), (-1, -1), 0.5, colors.HexColor('#cccccc')),
            ('ROWBACKGROUNDS', (0, 1), (-1, 1), [colors.HexColor('#f0f4f7')]),
        ]))
        return t

    def _create_health_meta_table(self, meta_lines: List[str]):
        full_text = " ".join(meta_lines)
        pairs = re.findall(r'([A-Za-z0-9_]+)=([^\s|]+)', full_text)
        
        friendly_names = {
            "DevID": "Device ID",
            "OpType": "Op Type",
            "CfgType": "Config Type",
            "StatusEnable": "Status Enable",
            "Mode": "Device Mode",
            "Ports": "Total Ports",
            "ConfigID": "Config ID",
            "SW_FIRMW": "SW Firmware",
            "ES_FIRMW": "ES Firmware",
            "VendorID": "Vendor ID",
            "Temp": "Temperature",
            "Volt": "Core Voltage",
            "TxTotal": "Total TX Pkts",
            "RxTotal": "Total RX Pkts",
            "TxErrTotal": "TX Errors",
            "RxErrTotal": "RX Errors"
        }

        op_type_map = {"0x57": "WRITE", "0x52": "READ", "0x53": "RTNET"}
        cfg_type_map = {"0x45": "SW REQUEST", "0x44": "SW MONITORING", "0x82": "SW MONITORING FINISH"}
        status_en_map = {"0x00": "Disabled", "0x01": "Enabled for Manager Switch", "0x03": "Enabled for Assistant FPGA", "0x05": "Enabled for MCU"}
        vendor_id_map = {"1": "MICROCHIP", "0": "Xilinx"}
        
        formatted_items = []
        for key, value in pairs:
            if key == "Mode": continue 
            
            label = friendly_names.get(key, key)
            formatted_val = value
            
            if key == "OpType" and value in op_type_map: formatted_val = op_type_map[value]
            elif key == "CfgType" and value in cfg_type_map: formatted_val = cfg_type_map[value]
            elif key == "StatusEnable" and value in status_en_map: formatted_val = status_en_map[value]
            elif key == "VendorID" and value in vendor_id_map: formatted_val = vendor_id_map[value]
            elif key == "DevID":
                try:
                    cleaned_val = value.rstrip('0')
                    if cleaned_val == '0x': cleaned_val = '0x0'
                    dec_val = int(cleaned_val, 16) if cleaned_val.startswith('0x') else int(cleaned_val)
                    formatted_val = f"{dec_val}" 
                except ValueError:
                    pass
            elif key in ["TxTotal", "RxTotal", "TxErrTotal", "RxErrTotal"]:
                try: formatted_val = f"{int(value):,}"
                except ValueError: pass
            elif key == "Temp" and value.endswith('C'): formatted_val = f"{value[:-1]} °C"
            elif key == "Volt" and value.endswith('V'): formatted_val = f"{value[:-1]} V"
                
            formatted_items.append(f"{label}: {formatted_val}")
        
        while len(formatted_items) < 16: formatted_items.append("-")
        formatted_items = formatted_items[:16]
        
        table_data = [formatted_items[0:4], formatted_items[4:8], formatted_items[8:12], formatted_items[12:16]]
        flowable_data = []
        for row in table_data:
            new_row = []
            for cell in row:
                new_row.append(ShrinkToFit(cell, font_name='Helvetica-Bold', font_size=8, text_color=colors.HexColor('#222222')))
            flowable_data.append(new_row)
            
        t = Table(flowable_data, colWidths=[1.8*inch]*4)
        t.setStyle(TableStyle([
            ('ALIGN', (0, 0), (-1, -1), 'CENTER'), ('VALIGN', (0, 0), (-1, -1), 'MIDDLE'),
            ('BOTTOMPADDING', (0, 0), (-1, -1), 6), ('TOPPADDING', (0, 0), (-1, -1), 6),
            ('GRID', (0, 0), (-1, -1), 0.5, colors.HexColor('#2c5aa0')),
            ('BACKGROUND', (0, 0), (-1, -1), colors.HexColor('#f0f4f7')),
        ]))
        return t

    def _create_validation_table(self, metadata):
        """Create a validation results table showing FW version check and 28V power status check results."""
        validation_rows = []
        fw_result = metadata.get("_fw_check_result")
        ps_result = metadata.get("_power_status_check_result")
        if fw_result:
            validation_rows.append(["FW Version Check", fw_result["status"], fw_result.get("detail", "")])
        if ps_result:
            exp = ps_result.get("expected", "")
            act = ps_result.get("actual", "")
            detail = f"Expected: {exp} | Actual: {act}" if exp else ""
            validation_rows.append(["28V Power Status Check", ps_result["status"], detail])
        if not validation_rows:
            return None

        v_headers = ["Check", "Result", "Details"]
        v_header_row = [ShrinkToFit(h, font_name='Helvetica-Bold', font_size=8, text_color=colors.whitesmoke) for h in v_headers]
        v_data = [v_header_row]
        for row in validation_rows:
            check_name = ShrinkToFit(row[0], font_name='Helvetica-Bold', font_size=8, text_color=colors.HexColor('#222222'))
            if row[1] == "PASSED":
                result_flow = ShrinkToFit(row[1], font_name='Helvetica-Bold', font_size=8, text_color=colors.HexColor('#00AA00'))
            else:
                result_flow = ShrinkToFit(row[1], font_name='Helvetica-Bold', font_size=8, text_color=colors.HexColor('#DD0000'))
            detail_flow = ShrinkToFit(row[2], font_name='Helvetica', font_size=7, text_color=colors.HexColor('#222222'))
            v_data.append([check_name, result_flow, detail_flow])
        v_table = Table(v_data, colWidths=[120, 60, 320])
        v_table.setStyle(TableStyle([
            ('BACKGROUND', (0, 0), (-1, 0), colors.HexColor('#2c5aa0')),
            ('ALIGN', (0, 0), (-1, -1), 'CENTER'),
            ('VALIGN', (0, 0), (-1, -1), 'MIDDLE'),
            ('BOTTOMPADDING', (0, 0), (-1, -1), 5),
            ('TOPPADDING', (0, 0), (-1, -1), 5),
            ('GRID', (0, 0), (-1, -1), 0.5, colors.HexColor('#cccccc')),
            ('ROWBACKGROUNDS', (0, 1), (-1, -1), [colors.white, colors.HexColor('#f9f9f9')]),
        ]))
        return v_table

    def create_table(self, headers, data, col_widths=None, font_size=7):
        if not data: return Paragraph("No data available.", self.styles['EnhancedBody'])
        formatted_data = []
        header_row = [ShrinkToFit(h, font_name='Helvetica-Bold', font_size=font_size+1, text_color=colors.whitesmoke) for h in headers]
        formatted_data.append(header_row)
        for row in data:
            new_row = [ShrinkToFit(cell, font_name='Helvetica', font_size=font_size, text_color=colors.black) for cell in row]
            formatted_data.append(new_row)
        t = Table(formatted_data, colWidths=col_widths)
        t.setStyle(TableStyle([
            ('BACKGROUND', (0, 0), (-1, 0), colors.HexColor('#2c5aa0')), ('ALIGN', (0, 0), (-1, -1), 'CENTER'),
            ('VALIGN', (0, 0), (-1, -1), 'MIDDLE'), ('BOTTOMPADDING', (0, 0), (-1, -1), 4),
            ('TOPPADDING', (0, 0), (-1, -1), 4), ('LEFTPADDING', (0, 0), (-1, -1), 1),
            ('RIGHTPADDING', (0, 0), (-1, -1), 1), ('GRID', (0, 0), (-1, -1), 0.5, colors.HexColor('#cccccc')),
            ('ROWBACKGROUNDS', (0, 1), (-1, -1), [colors.white, colors.HexColor('#f9f9f9')]),
        ]))
        return t

    def _build_single_pass(self, doc, story):
        def on_page(canvas_obj, doc_obj):
            is_cover = (doc_obj.page == 1)
            if is_cover: self._draw_cover_page_border(canvas_obj, doc_obj)
            self._create_header_footer(canvas_obj, doc_obj, is_cover_page=is_cover)
        doc.build(story, onFirstPage=on_page, onLaterPages=on_page)

    def generate_pdf_chunk(self, chunk_data: Dict, output_file: str, is_first_chunk: bool, chunk_idx: int, total_chunks: int, summary_table: List = None, summary_ptp_table: List = None, summary_health: Dict = None):
        doc = SimpleDocTemplate(output_file, pagesize=letter, rightMargin=45, leftMargin=45, topMargin=110, bottomMargin=90)
        story = []

        self.apply_metadata(chunk_data["metadata"])
        self.chunk_info = f"(Part {chunk_idx+1}/{total_chunks})" if total_chunks > 1 else ""
        
        if is_first_chunk:
            story.extend(self._create_cover_page())
            story.extend(self._create_product_info_section(chunk_data["test_duration"]))
            
            # Summary 1: Port Statistics
            if summary_table:
                story.append(Paragraph("Test Summary (Port Statistics)", self.styles['PhaseTitle']))
                m_headers = ["Port", "TX Pkts", "TX Bytes", "TX Gbps", "RX Pkts", "RX Bytes", "RX Gbps", "Good", "Bad", "Lost", "Bit Err", "BER"]
                col_w = [30, 50, 60, 40, 50, 60, 40, 50, 35, 35, 38, 34] 
                story.append(self.create_table(m_headers, summary_table, col_widths=col_w, font_size=6.5))
                story.append(PageBreak())

            # Summary 2: PTP Statistics
            if summary_ptp_table:
                story.append(Paragraph("Test Summary (PTP Statistics)", self.styles['PhaseTitle']))
                ptp_headers = ["Port", "ReqVL", "RespVL", "State", "Offset(ns)", "Delay(ns)", "Sync RX", "Req TX", "Resp RX", "Synced"]
                ptp_col_w = [30, 45, 45, 60, 90, 55, 50, 45, 50, 45]
                story.append(self.create_table(ptp_headers, summary_ptp_table, col_widths=ptp_col_w, font_size=7.5))
                story.append(PageBreak())

            # Summary 3: Assistant FPGA
            if summary_health and (summary_health.get("ast_meta") or summary_health.get("ast_table")):
                story.append(Paragraph("Test Summary (ASSISTANT FPGA)", self.styles['PhaseTitle']))
                if summary_health.get("ast_meta"):
                    story.append(self._create_health_meta_table(summary_health["ast_meta"]))
                    story.append(Spacer(1, 15))
                if summary_health.get("ast_table"):
                    h_headers = ["Port", "TxCnt", "RxCnt", "PolDrop", "VLDrop", "HP_Ovf", "LP_Ovf", "BE_Ovf"]
                    story.append(self.create_table(h_headers, summary_health["ast_table"], col_widths=[40, 75, 75, 55, 55, 55, 55, 55], font_size=7.5))
                story.append(PageBreak())

            # Summary 4: Manager FPGA
            if summary_health and (summary_health.get("mgr_meta") or summary_health.get("mgr_table")):
                story.append(Paragraph("Test Summary (MANAGER FPGA)", self.styles['PhaseTitle']))
                if summary_health.get("mgr_meta"):
                    story.append(self._create_health_meta_table(summary_health["mgr_meta"]))
                    story.append(Spacer(1, 15))
                if summary_health.get("mgr_table"):
                    h_headers = ["Port", "TxCnt", "RxCnt", "PolDrop", "VLDrop", "HP_Ovf", "LP_Ovf", "BE_Ovf"]
                    story.append(self.create_table(h_headers, summary_health["mgr_table"], col_widths=[40, 75, 75, 55, 55, 55, 55, 55], font_size=7.5))
                story.append(PageBreak())

            # Summary 5: MCU Status
            story.append(Paragraph("Test Summary (MCU Status)", self.styles['PhaseTitle']))
            has_mcu_data = summary_health and any([summary_health.get("mcu_meta_general"), summary_health.get("mcu_meta_status"), summary_health.get("mcu_curr_table"), summary_health.get("mcu_volt_table"), summary_health.get("mcu_meta_temp")])
            if has_mcu_data:
                mcu_source = summary_health.get("mcu_source", "")
                if mcu_source:
                    story.append(Paragraph(f"Last available MCU data from: {mcu_source}", self.styles['EnhancedBody']))
                    story.append(Spacer(1, 5))
                if summary_health.get("mcu_meta_general"):
                    t_gen = self._create_key_val_table(summary_health["mcu_meta_general"])
                    if t_gen:
                        story.append(t_gen)
                        story.append(Spacer(1, 10))
                if summary_health.get("mcu_meta_status"):
                    t_stat = self._create_key_val_table(summary_health["mcu_meta_status"])
                    if t_stat:
                        story.append(t_stat)
                        story.append(Spacer(1, 10))
                if summary_health.get("mcu_curr_table"):
                    curr_data = [list(row) for row in summary_health["mcu_curr_table"]]
                    for row in curr_data:
                        if "FO Trans1" in row[0]: row[0] = row[0].replace("FO Trans1", "FO Transceiver")
                    story.append(Paragraph("Current Data", self.styles['SubTitle']))
                    story.append(self.create_table(["Rail", "Current (A)"], curr_data, col_widths=[140, 90], font_size=8))
                    story.append(Spacer(1, 10))
                if summary_health.get("mcu_volt_table"):
                    volt_data = [list(row) for row in summary_health["mcu_volt_table"]]
                    for row in volt_data:
                        if "FO Trans1" in row[0]: row[0] = row[0].replace("FO Trans1", "FO Transceiver")
                    story.append(Paragraph("Voltage Data", self.styles['SubTitle']))
                    story.append(self.create_table(["Rail", "Voltage (V)"], volt_data, col_widths=[140, 90], font_size=8))
                    story.append(Spacer(1, 10))
                if summary_health.get("mcu_meta_temp"):
                    story.append(Paragraph("Temperatures", self.styles['SubTitle']))
                    t_temp = self._create_key_val_table(summary_health["mcu_meta_temp"])
                    if t_temp: story.append(t_temp)
            else:
                story.append(Paragraph("WARNING: No MCU data was received during the entire test.", self.styles['EnhancedBody']))
            # Validation Results in Summary MCU
            v_table = self._create_validation_table(chunk_data["metadata"])
            if v_table:
                story.append(Spacer(1, 10))
                story.append(Paragraph("Validation Results", self.styles['SubTitle']))
                story.append(v_table)
            story.append(PageBreak())

        if chunk_data["phases"]:
            for phase in chunk_data["phases"]:
                
                # ==================== PAGE 1: PORT STATS ====================
                story.append(Paragraph(f"{phase['name']} (Port Statistics)", self.styles['PhaseTitle']))
                if phase.get("main_table"):
                    m_headers = ["Port", "TX Pkts", "TX Bytes", "TX Gbps", "RX Pkts", "RX Bytes", "RX Gbps", "Good", "Bad", "Lost", "Bit Err", "BER"]
                    story.append(self.create_table(m_headers, phase["main_table"], col_widths=[30, 50, 60, 40, 50, 60, 40, 50, 35, 35, 38, 34], font_size=6.5))
                    story.append(Spacer(1, 10)) 
                if phase.get("raw_multi_table"):
                    story.append(Paragraph("Raw Socket Multi-Target Statistics", self.styles['SubTitle']))
                    rs_headers = ["Source", "Target", "Rate", "TX Pkts", "TX Mbps", "RX Pkts", "Good", "Bad", "Lost", "Bit Err", "BER"]
                    story.append(self.create_table(rs_headers, phase["raw_multi_table"], col_widths=[40, 45, 55, 50, 50, 50, 50, 35, 35, 50, 62], font_size=6.5))
                    story.append(Spacer(1, 10))
                if phase.get("port12_table"):
                    story.append(Paragraph("Port 12 RX: DPDK External TX Packets", self.styles['SubTitle']))
                    story.append(self.create_table(["RX Pkts", "RX Mbps", "Good", "Bad", "Bit Errors", "Lost", "BER"], phase["port12_table"], col_widths=[80, 80, 80, 60, 80, 60, 82], font_size=7))
                    story.append(Spacer(1, 5))
                if phase.get("port13_table"):
                    story.append(Paragraph("Port 13 RX: DPDK External TX Packets", self.styles['SubTitle']))
                    story.append(self.create_table(["RX Pkts", "RX Mbps", "Good", "Bad", "Bit Errors", "Lost", "BER"], phase["port13_table"], col_widths=[80, 80, 80, 60, 80, 60, 82], font_size=7))
                
                if not any([phase.get("main_table"), phase.get("raw_multi_table"), phase.get("port12_table"), phase.get("port13_table")]):
                    story.append(Paragraph("No Port Statistics available for this phase.", self.styles['EnhancedBody']))
                
                story.append(PageBreak()) 
                
                # ==================== PAGE 2: PTP STATS ====================
                story.append(Paragraph(f"{phase['name']} (PTP Statistics)", self.styles['PhaseTitle']))
                if phase.get("ptp_table"):
                    ptp_headers = ["Port", "ReqVL", "RespVL", "State", "Offset(ns)", "Delay(ns)", "Sync RX", "Req TX", "Resp RX", "Synced"]
                    ptp_col_w = [30, 45, 45, 60, 90, 55, 50, 45, 50, 45]
                    story.append(self.create_table(ptp_headers, phase["ptp_table"], col_widths=ptp_col_w, font_size=7.5))
                else:
                    story.append(Paragraph("No PTP Statistics available.", self.styles['EnhancedBody']))
                story.append(PageBreak())

                # ==================== PAGE 3: ASSISTANT FPGA ====================
                story.append(Paragraph(f"{phase['name']} (ASSISTANT FPGA Monitor)", self.styles['PhaseTitle']))
                if phase.get("ast_meta") or phase.get("ast_table"):
                    if phase.get("ast_meta"):
                        story.append(self._create_health_meta_table(phase["ast_meta"]))
                        story.append(Spacer(1, 15))
                    if phase.get("ast_table"):
                        h_headers = ["Port", "TxCnt", "RxCnt", "PolDrop", "VLDrop", "HP_Ovf", "LP_Ovf", "BE_Ovf"]
                        story.append(self.create_table(h_headers, phase["ast_table"], col_widths=[40, 75, 75, 55, 55, 55, 55, 55], font_size=7.5))
                else:
                    story.append(Paragraph("No Assistant FPGA data available for this phase.", self.styles['EnhancedBody']))
                story.append(PageBreak()) 

                # ==================== PAGE 4: MANAGER FPGA ====================
                story.append(Paragraph(f"{phase['name']} (MANAGER FPGA Monitor)", self.styles['PhaseTitle']))
                if phase.get("mgr_meta") or phase.get("mgr_table"):
                    if phase.get("mgr_meta"):
                        story.append(self._create_health_meta_table(phase["mgr_meta"]))
                        story.append(Spacer(1, 15))
                    if phase.get("mgr_table"):
                        h_headers = ["Port", "TxCnt", "RxCnt", "PolDrop", "VLDrop", "HP_Ovf", "LP_Ovf", "BE_Ovf"]
                        story.append(self.create_table(h_headers, phase["mgr_table"], col_widths=[40, 75, 75, 55, 55, 55, 55, 55], font_size=7.5))
                        
                        if any(row[0].strip() == "34" for row in phase["mgr_table"]):
                            story.append(Spacer(1, 8))
                            manager_ek_notlar = """
                            <i><b>* Note for Port 34:</b> This port is currently active and processing internal management traffic.</i><br/>
                            <i><b>* HP_Ovf:</b> High Priority Overflow</i><br/>
                            <i><b>* LP_Ovf:</b> Low Priority Overflow</i><br/>
                            <i><b>* BE_Ovf:</b> Best Effort Overflow</i>
                            """
                            story.append(Paragraph(manager_ek_notlar, self.styles['EnhancedBody']))
                else:
                    story.append(Paragraph("No Manager FPGA data available for this phase.", self.styles['EnhancedBody']))
                story.append(PageBreak())

                # ==================== PAGE 5: MCU STATUS ====================
                has_mcu = any([phase.get("mcu_meta_general"), phase.get("mcu_meta_status"), phase.get("mcu_curr_table"), phase.get("mcu_volt_table"), phase.get("mcu_meta_temp")])
                if has_mcu:
                    story.append(Paragraph(f"{phase['name']} (MCU Status)", self.styles['PhaseTitle']))
                    
                    if phase.get("mcu_meta_general"):
                        t_gen = self._create_key_val_table(phase["mcu_meta_general"])
                        if t_gen:
                            story.append(t_gen)
                            story.append(Spacer(1, 10))
                            
                    if phase.get("mcu_meta_status"):
                        t_stat = self._create_key_val_table(phase["mcu_meta_status"])
                        if t_stat:
                            story.append(t_stat)
                            story.append(Spacer(1, 10))
                            
                    if phase.get("mcu_curr_table"):
                        for row in phase["mcu_curr_table"]:
                            if "FO Trans1" in row[0]: row[0] = row[0].replace("FO Trans1", "FO Transceiver")
                        story.append(Paragraph("Current Data", self.styles['SubTitle']))
                        story.append(self.create_table(["Rail", "Current (A)"], phase["mcu_curr_table"], col_widths=[140, 90], font_size=8))
                        story.append(Spacer(1, 10))

                    if phase.get("mcu_volt_table"):
                        for row in phase["mcu_volt_table"]:
                            if "FO Trans1" in row[0]: row[0] = row[0].replace("FO Trans1", "FO Transceiver")
                        story.append(Paragraph("Voltage Data", self.styles['SubTitle']))
                        story.append(self.create_table(["Rail", "Voltage (V)"], phase["mcu_volt_table"], col_widths=[140, 90], font_size=8))
                        story.append(Spacer(1, 10))

                    if phase.get("mcu_meta_temp"):
                        story.append(Paragraph("Temperatures", self.styles['SubTitle']))
                        t_temp = self._create_key_val_table(phase["mcu_meta_temp"])
                        if t_temp: story.append(t_temp)

                    # Validation Results Table
                    v_table = self._create_validation_table(chunk_data["metadata"])
                    if v_table:
                        story.append(Spacer(1, 10))
                        story.append(Paragraph("Validation Results", self.styles['SubTitle']))
                        story.append(v_table)

                else:
                    story.append(Paragraph(f"{phase['name']} (MCU Status)", self.styles['PhaseTitle']))
                    story.append(Paragraph("No MCU Status data available for this phase.", self.styles['EnhancedBody']))
                
                story.append(PageBreak())

        self._build_single_pass(doc, story)

# ==========================================
# MULTIPROCESSING WORKER
# ==========================================
def worker_generate_pdf(args):
    idx, total_chunks, phases_chunk, metadata, test_dur, base_output, logo_path, summary_table, summary_ptp_table, summary_health = args

    out_file = f"{base_output[:-4]}_part{idx+1}.pdf" if base_output.lower().endswith('.pdf') else f"{base_output}_part{idx+1}.pdf"
    out_dir = os.path.dirname(out_file)
    if out_dir: os.makedirs(out_dir, exist_ok=True)

    debug_print(f"   -> [Worker {idx+1}/{total_chunks}] Started: processing {len(phases_chunk)} tests... (5-Page Layout)")

    pdf_gen = PDFReportTemplate(logo_path=logo_path)
    chunk_data = {"metadata": metadata, "test_duration": test_dur, "phases": phases_chunk}

    pdf_gen.generate_pdf_chunk(chunk_data, out_file, is_first_chunk=(idx==0), chunk_idx=idx, total_chunks=total_chunks, summary_table=summary_table, summary_ptp_table=summary_ptp_table, summary_health=summary_health)
    debug_print(f"   ✓ [Worker {idx+1}/{total_chunks}] Completed: {out_file}")
    return out_file

# ==========================================
# MAIN EXECUTION
# ==========================================
def main():
    parser = argparse.ArgumentParser(description='Generate Turbo Optimized PDF from Massive DPDK Log')
    parser.add_argument('-i', '--input', required=True, help='Absolute path to the input log file')
    parser.add_argument('-o', '--output', default='dtn.pdf', help='Output PDF filename')
    parser.add_argument('--logo', default=DEFAULT_LOGO_PATH, help='Path to logo image')
    parser.add_argument('--chunk-size', type=int, default=2500, help='Number of tests per PDF file (Default: 2500)')
    args = parser.parse_args()

    input_file = args.input

    if not os.path.exists(input_file):
        print(f"ERROR: Specified log file not found: '{input_file}'")
        sys.exit(1)

    start_time = datetime.now()
    debug_print(f"[{start_time.strftime('%H:%M:%S')}] Parsing log file: {input_file}")
    
    log_parser = LogParser(input_file)
    parsed_data = log_parser.parse()
    
    total_phases = len(parsed_data['phases'])
    formatted_duration = format_duration(parsed_data.get('test_duration', 'N/A'))
    
    # Summary tabloları: her veri türü için bağımsız olarak son geçerli veriyi bul.
    summary_table = None
    summary_ptp_table = None
    summary_ast_meta = None
    summary_ast_table = None
    summary_mgr_meta = None
    summary_mgr_table = None
    summary_mcu_meta_general = None
    summary_mcu_meta_status = None
    summary_mcu_curr_table = None
    summary_mcu_volt_table = None
    summary_mcu_meta_temp = None
    summary_mcu_source = None  # MCU verisinin hangi fazdan geldiğini takip et

    for phase in reversed(parsed_data["phases"]):
        if not summary_table and phase.get("main_table"):
            summary_table = phase.get("main_table")
        if not summary_ptp_table and phase.get("ptp_table"):
            summary_ptp_table = phase.get("ptp_table")
        if not summary_ast_meta and phase.get("ast_meta"):
            summary_ast_meta = phase.get("ast_meta")
        if not summary_ast_table and phase.get("ast_table"):
            summary_ast_table = phase.get("ast_table")
        if not summary_mgr_meta and phase.get("mgr_meta"):
            summary_mgr_meta = phase.get("mgr_meta")
        if not summary_mgr_table and phase.get("mgr_table"):
            summary_mgr_table = phase.get("mgr_table")
        if not summary_mcu_curr_table and phase.get("mcu_curr_table"):
            summary_mcu_meta_general = phase.get("mcu_meta_general")
            summary_mcu_meta_status = phase.get("mcu_meta_status")
            summary_mcu_curr_table = phase.get("mcu_curr_table")
            summary_mcu_volt_table = phase.get("mcu_volt_table")
            summary_mcu_meta_temp = phase.get("mcu_meta_temp")
            summary_mcu_source = phase.get("name", "?")
        # Hepsi bulunduysa erken çık
        if all([summary_table, summary_ptp_table, summary_ast_table, summary_mgr_table, summary_mcu_curr_table]):
            break
    
    debug_print(f"[{datetime.now().strftime('%H:%M:%S')}] Parsed data summary:")
    debug_print(f"   - Metadata Fields: {len(parsed_data['metadata'])}")
    debug_print(f"   - Synced Test Duration: {formatted_duration}")
    debug_print(f"   - Total Tests Selected for PDF: {total_phases}")
    debug_print(f"   - Validation Reference Phase: {parsed_data.get('reference_phase_name')}")
    debug_print(f"   - Calculated Test Result: {parsed_data['metadata'].get('Test Result', 'Unknown')}")

    if total_phases == 0:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] WARNING: No test phases found to print.")
        sys.exit(1)

    chunks = [parsed_data["phases"][i:i + args.chunk_size] for i in range(0, total_phases, args.chunk_size)]
    total_chunks = len(chunks)
    
    summary_health = {
        "ast_meta": summary_ast_meta,
        "ast_table": summary_ast_table,
        "mgr_meta": summary_mgr_meta,
        "mgr_table": summary_mgr_table,
        "mcu_meta_general": summary_mcu_meta_general,
        "mcu_meta_status": summary_mcu_meta_status,
        "mcu_curr_table": summary_mcu_curr_table,
        "mcu_volt_table": summary_mcu_volt_table,
        "mcu_meta_temp": summary_mcu_meta_temp,
        "mcu_source": summary_mcu_source,
    }
    worker_tasks = [(i, total_chunks, chunk, parsed_data["metadata"], formatted_duration, args.output, args.logo, summary_table, summary_ptp_table, summary_health) for i, chunk in enumerate(chunks)]

    cpu_cores = max(1, mp.cpu_count() - 1)
    debug_print(f"[{datetime.now().strftime('%H:%M:%S')}] Generating {total_chunks} PDF chunk(s) using {cpu_cores} core(s)...")

    with mp.Pool(processes=cpu_cores) as pool:
        generated_parts = pool.map(worker_generate_pdf, worker_tasks)

    if len(generated_parts) == 1:
        os.rename(generated_parts[0], args.output)
    else:
        try:
            from pypdf import PdfWriter
            merger = PdfWriter()
            for pdf in generated_parts: merger.append(pdf)
            merger.write(args.output)
            merger.close()
            for pdf in generated_parts: os.remove(pdf)
        except ImportError:
            import shutil
            shutil.move(generated_parts[0], args.output)

    duration = (datetime.now() - start_time).total_seconds()
    print(f"[{datetime.now().strftime('%H:%M:%S')}] SUCCESS! Process completed in {duration:.2f} seconds.")

if __name__ == '__main__':
    main()