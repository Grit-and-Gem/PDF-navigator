#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

void upsertMapping(const fs::path& mappingFile, const std::string& pdfName, const std::string& uniqueCode);

std::string escapeJson(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

std::string makeUniqueCode() {
    static std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<unsigned long long> dist;

    auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    unsigned long long randomPart = dist(rng);

    std::stringstream ss;
    ss << std::hex << now << randomPart;
    std::string code = ss.str();
    if (code.size() > 16) {
        code = code.substr(0, 16);
    }
    return code;
}

std::string unescapeJson(const std::string& value) {
    std::string unescaped;
    unescaped.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            ++i;
            switch (value[i]) {
                case 'n': unescaped += '\n'; break;
                case 'r': unescaped += '\r'; break;
                case 't': unescaped += '\t'; break;
                case '\\': unescaped += '\\'; break;
                case '"': unescaped += '"'; break;
                default: unescaped += value[i]; break;
            }
        } else {
            unescaped += value[i];
        }
    }
    return unescaped;
}

std::unordered_map<std::string, std::string> readMapping(const fs::path& mappingFile) {
    std::unordered_map<std::string, std::string> mapping;
    std::ifstream in(mappingFile);
    if (!in.is_open()) {
        return mapping;
    }

    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::regex pairRegex("\\\"((?:\\\\.|[^\\\"])*)\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
    std::sregex_iterator begin(content.begin(), content.end(), pairRegex);
    std::sregex_iterator end;

    for (auto it = begin; it != end; ++it) {
        std::string key = unescapeJson((*it)[1].str());
        std::string value = unescapeJson((*it)[2].str());
        mapping[key] = value;
    }

    return mapping;
}

bool hasPngInFolder(const fs::path& folder) {
    if (!fs::exists(folder) || !fs::is_directory(folder)) {
        return false;
    }

    for (const auto& entry : fs::directory_iterator(folder)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string ext = entry.path().extension().string();
        for (char& ch : ext) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (ext == ".png") {
            return true;
        }
    }
    return false;
}

bool isAlreadyConverted(const std::string& pdfFileName,
                        const std::unordered_map<std::string, std::string>& mapping,
                        const fs::path& imageDir) {
    auto it = mapping.find(pdfFileName);
    if (it == mapping.end() || it->second.empty()) {
        return false;
    }
    return hasPngInFolder(imageDir / it->second);
}

int convertSinglePdf(const fs::path& pdfPath,
                     const std::string& pdfFileName,
                     const std::string& uniqueCode,
                     int dpi,
                     const fs::path& imageDir,
                     const fs::path& configFile) {
    fs::path outputFolder = imageDir / uniqueCode;
    fs::create_directories(outputFolder);

    fs::path outputPattern = outputFolder / "page-%d.png";
    std::string command = "mutool draw -q -r " + std::to_string(dpi) + " -F png -o \"" + outputPattern.string() + "\" \"" + pdfPath.string() + "\"";
    int result = std::system(command.c_str());
    if (result != 0) {
        std::cerr << "Conversion failed for: " << pdfPath << "\n";
        return result;
    }

    upsertMapping(configFile, pdfFileName, uniqueCode);
    std::cout << "Converted: " << pdfPath << "\n";
    std::cout << "Images saved in: " << outputFolder << "\n";
    return 0;
}

void upsertMapping(const fs::path& mappingFile, const std::string& pdfName, const std::string& uniqueCode) {
    std::ifstream in(mappingFile);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    std::string escapedName = escapeJson(pdfName);
    std::string escapedCode = escapeJson(uniqueCode);
    std::string pairLine = "\"" + escapedName + "\": \"" + escapedCode + "\"";

    if (content.empty()) {
        content = "{}\n";
    }

    std::regex keyRegex("(\\\"" + escapedName + "\\\"\\s*:\\s*\\\")(.*?)(\\\")");
    if (std::regex_search(content, keyRegex)) {
        content = std::regex_replace(content, keyRegex, "$1" + escapedCode + "$3");
    } else {
        std::size_t closeBrace = content.rfind('}');
        if (closeBrace == std::string::npos) {
            content = "{}\n";
            closeBrace = content.rfind('}');
        }

        std::string before = content.substr(0, closeBrace);
        std::string trimmed = before;
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == '\t')) {
            trimmed.pop_back();
        }

        bool hasAnyPair = trimmed.size() > 1;
        std::string insertText = hasAnyPair ? ",\n  " + pairLine + "\n" : "\n  " + pairLine + "\n";
        content = before + insertText + "}\n";
    }

    std::ofstream out(mappingFile, std::ios::trunc);
    out << content;
}

bool parsePositiveInt(const std::string& input, int& valueOut) {
    try {
        std::size_t idx = 0;
        int parsed = std::stoi(input, &idx);
        if (idx != input.size() || parsed <= 0) {
            return false;
        }
        valueOut = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void printUsage(const std::string& programName) {
    std::cout << "Usage: " << programName << " [--dpi <number>] [--force]\n";
    std::cout << "  --dpi <number>  Render resolution in DPI (default: 150)\n";
    std::cout << "  --force         Re-convert already converted PDFs\n";
}

int main(int argc, char* argv[]) {
    fs::path root = fs::current_path();
    fs::path pdfDir = root / "pdf";
    fs::path imageDir = root / "image";
    fs::path configFile = root / "config" / "pdf_image_map.json";
    int dpi = 150;
    bool force = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--force") {
            force = true;
            continue;
        }

        if (arg == "--dpi") {
            if (i + 1 >= argc || !parsePositiveInt(argv[i + 1], dpi)) {
                std::cerr << "Invalid value for --dpi. Use a positive integer.\n";
                printUsage(argv[0]);
                return 1;
            }
            ++i;
            continue;
        }

        if (arg.rfind("--dpi=", 0) == 0) {
            std::string value = arg.substr(6);
            if (!parsePositiveInt(value, dpi)) {
                std::cerr << "Invalid value for --dpi. Use a positive integer.\n";
                printUsage(argv[0]);
                return 1;
            }
            continue;
        }

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }

        std::cerr << "Unknown option: " << arg << "\n";
        printUsage(argv[0]);
        return 1;
    }

    if (!fs::exists(pdfDir) || !fs::is_directory(pdfDir)) {
        std::cerr << "PDF directory not found: " << pdfDir << "\n";
        return 1;
    }

    fs::create_directories(imageDir);
    fs::create_directories(configFile.parent_path());

    if (!fs::exists(configFile)) {
        std::ofstream initConfig(configFile);
        initConfig << "{}\n";
    }

    if (std::system("command -v mutool >/dev/null 2>&1") != 0) {
        std::cerr << "Conversion failed. Ensure mutool is installed (mupdf-tools).\n";
        return 1;
    }

    auto mapping = readMapping(configFile);
    std::vector<std::pair<std::string, fs::path>> pending;

    for (const auto& entry : fs::directory_iterator(pdfDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        std::string ext = entry.path().extension().string();
        for (char& ch : ext) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (ext != ".pdf") {
            continue;
        }

        std::string pdfFileName = entry.path().filename().string();
        if (force || !isAlreadyConverted(pdfFileName, mapping, imageDir)) {
            pending.push_back({pdfFileName, entry.path()});
        }
    }

    if (pending.empty()) {
        if (force) {
            std::cout << "No PDF files found in: " << pdfDir << "\n";
        } else {
            std::cout << "No unconverted PDF files found in: " << pdfDir << "\n";
        }
        return 0;
    }

    std::cout << "DPI set to: " << dpi << "\n";
    if (force) {
        std::cout << "Force mode enabled: already converted files will be converted again.\n";
    }

    std::cout << "Found " << pending.size() << (force ? " PDF file(s)" : " unconverted PDF file(s)") << ":\n";
    for (const auto& item : pending) {
        std::cout << "- " << item.first << "\n";
    }

    std::cout << "Convert these files now? (y/N): ";
    std::string answer;
    std::getline(std::cin, answer);
    if (answer != "y" && answer != "Y") {
        std::cout << "Conversion cancelled.\n";
        return 0;
    }

    int convertedCount = 0;
    int failedCount = 0;

    for (const auto& item : pending) {
        const std::string& pdfFileName = item.first;
        const fs::path& pdfPath = item.second;

        std::string uniqueCode;
        auto mapped = mapping.find(pdfFileName);
        if (force && mapped != mapping.end() && !mapped->second.empty()) {
            uniqueCode = mapped->second;
        } else {
            uniqueCode = makeUniqueCode();
        }

        fs::path outputFolder = imageDir / uniqueCode;
        if (force && fs::exists(outputFolder) && fs::is_directory(outputFolder)) {
            for (const auto& existing : fs::directory_iterator(outputFolder)) {
                if (existing.is_regular_file()) {
                    std::string ext = existing.path().extension().string();
                    for (char& ch : ext) {
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    }
                    if (ext == ".png") {
                        fs::remove(existing.path());
                    }
                }
            }
        }

        int result = convertSinglePdf(pdfPath, pdfFileName, uniqueCode, dpi, imageDir, configFile);
        if (result == 0) {
            ++convertedCount;
            mapping[pdfFileName] = uniqueCode;
        } else {
            ++failedCount;
        }
    }

    std::cout << "Mapping updated in: " << configFile << "\n";
    std::cout << "Summary: converted=" << convertedCount << ", failed=" << failedCount << "\n";

    return failedCount == 0 ? 0 : 1;
}
