#ifndef REPORT_MANAGER_H
#define REPORT_MANAGER_H

#include <string>

class ReportManager
{
public:
    ReportManager();
    ~ReportManager();

    // Collects test information from user
    bool collectTestInfo();

    // Returns the test name
    std::string getTestName() const;
    std::string getTesterName() const;
    std::string getQualityCheckerName() const;
    std::string getSerialNumber() const;
    std::string getBilgemNumber() const;
    std::string getAteSerialNumber() const;

    void setUnitName(std::string name);

    // Writes report header to the beginning of the log file
    bool writeReportHeader();

    // Creates PDF report from log file
    bool createPdfReport();

    // Timestamp recording methods
    void recordSoftwareStartTime();
    void recordUnitPowerOnTime();
    void recordPowerOffTime();
    void recordSoftwareEndTime();

    // Returns current timestamp as formatted string
    static std::string getCurrentTimestamp();

private:
    // Returns the ReportGenerator binary path (compiled from Python via PyInstaller)
    std::string getPythonScriptPath() const;
    // Checks if input contains Turkish characters
    bool containsTurkishCharacter(const std::string &input) const;

    // Checks if input contains only digits
    bool containsOnlyDigits(const std::string &input) const;

    // Returns the log directory path for the given unit name
    std::string getLogPathForUnit() const;

    std::string m_testName;
    std::string m_test_name_correction;
    std::string m_ate_serial_number;
    std::string m_ate_serial_number_correction;
    std::string m_bilgem_number;
    std::string m_bilgem_number_correction;
    std::string m_serial_number;
    std::string m_serial_number_correction;
    std::string m_tester_name;
    std::string m_quality_checker_name;
    std::string m_unit_name;

    // Timestamp fields
    std::string m_software_start_time;
    std::string m_unit_power_on_time;
    std::string m_power_off_time;
    std::string m_software_end_time;
};

// Global singleton declaration
extern ReportManager g_ReportManager;

#endif // REPORT_MANAGER_H