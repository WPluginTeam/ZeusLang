// ben.cpp - micro benchmark comparing native C++ vs Zeus VM
// English only output (avoid encoding issues)

#include <iostream>
#include <chrono>
#include <unordered_map>
#include <string>
#include <vector>
#include <fstream>

#include "./ZeusCompile.hpp"
#include "./ZeusDebug/Utils.hpp"
#include "./ZeusRunner/ZeusBuiltins.hpp"

// Helper to run a Zeus source snippet and return final program + exec time
struct ZeusBenchResult {
	ZeusCompile::Result compileRes;
	double compileMs = 0.0;
	double execMs = 0.0;
};

struct EntryStat {
	std::string name;    // label
	double compileMs=0;  // for Zeus entries (0 for native)
	double execMs=0;     // main metric
	double extra=0;      // reserved / final value
};

static ZeusBenchResult runZeus(const std::string &src, bool strictDecl, bool printIR) {
	ZeusBenchResult r;
	auto t0 = std::chrono::steady_clock::now();
	r.compileRes = ZeusCompile::compile(src, strictDecl);
	auto t1 = std::chrono::steady_clock::now();
	if (r.compileRes.program.empty()) {
		std::cout << "[Zeus] compile failed\n";
		return r;
	}
	if (printIR) {
		std::cout << "-------------------- IR Gen --------------------" << std::endl;
		for (size_t ci = 0; ci < r.compileRes.program.size(); ++ci) {
			std::cout << "== Chunk #" << ci << " ==" << std::endl;
			for (auto &ins : r.compileRes.program[ci]) {
				std::cout << ins.toString() << std::endl;
			}
		}
		std::cout << "-------------------- Output --------------------" << std::endl;
	}
	auto t2 = std::chrono::steady_clock::now();
	ZeusCompile::run(r.compileRes); // run prints timing if global enabled, we also record manually
	auto t3 = std::chrono::steady_clock::now();
	r.compileMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
	r.execMs    = std::chrono::duration<double, std::milli>(t3 - t2).count();
	return r;
}

int main(int argc, char* argv[]) {
	using clock = std::chrono::steady_clock;
	// Args:
	//   ben.exe [N] [--runs=R] [--no-ir] [--no-delayprint]
	int N = 100000;
	int runs = 1; // repeat Zeus sections to average
	bool printIR = true;
	bool keepDelayPrint = true;
	for (int i=1;i<argc;i++) {
		std::string a = argv[i];
		if (a.rfind("--runs=",0)==0) { runs = std::stoi(a.substr(7)); continue; }
		if (a == "--no-ir") { printIR = false; continue; }
		if (a == "--no-delayprint") { keepDelayPrint = false; continue; }
		// first plain number -> N
		bool allDigit = !a.empty();
		for (char c: a) if (!isdigit((unsigned char)c)) { allDigit=false; break; }
		if (allDigit) { N = std::stoi(a); continue; }
		std::cout << "Unknown arg: " << a << "\n";
	}

	if (runs < 1) runs = 1;
	std::cout << "Benchmark: N=" << N << " runs=" << runs << "\n";

	// init builtins BEFORE any compile to avoid CALL_CLOSURE errors
	ZeusBuiltins::initCoreBuiltins();

	std::vector<EntryStat> stats; stats.reserve(8);

	// --------------------------------------------------
	// 1. Native C++ integer increment
	// --------------------------------------------------
	{
		volatile int x = 0; // volatile to avoid over-optimization collapse
		auto t0 = clock::now();
		for (int i = 0; i < N; ++i) {
			x = x + 1;
		}
		auto t1 = clock::now();
		double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		std::cout << "[C++] int increment: " << ms << " ms, final=" << x << "\n";
		stats.push_back({"C++_int", 0.0, ms, (double)x});
	}

	

	ZeusCompile::enableTiming = true; // show per-phase timing

	// --------------------------------------------------
	// 3. Zeus variable increment benchmark
	// --------------------------------------------------
	{
		std::string src;
		src += "var x = 0;\n";
		src += "for (" + std::to_string(N) + ") {\n";
		src += "    x = x + 1;\n";
		src += "}\n";              // close loop
		if (keepDelayPrint) src += "delayPrint(x);\n";
		std::cout << "\n[Zeus] variable increment source begin\n";
		std::cout << src;
		std::cout << "[Zeus] variable increment source end\n";
		double totCompile = 0.0, totExec = 0.0;
		for (int r=0;r<runs;r++) {
			auto zr = runZeus(src, false, printIR && r==0);
			totCompile += zr.compileMs;
			totExec    += zr.execMs;
		}
		double avgC = totCompile/runs;
		double avgE = totExec/runs;
		std::cout << "[Zeus] var inc avg compile(ms)=" << avgC
				  << " avg execute(ms)=" << avgE << "\n";
		stats.push_back({"Zeus_var_inc", avgC, avgE, 0});
	}

    // --------------------------------------------------
	// 2. Native C++ unordered_map<int,int> single key increment
	// --------------------------------------------------
	{
		std::unordered_map<int,int> m;
		m[0] = 4;
		auto t0 = clock::now();
		for (int i = 0; i < N; ++i) {
			m[0] = m[0] + 1;
		}
		auto t1 = clock::now();
		double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
		std::cout << "[C++] map[0] increment: " << ms << " ms, final=" << m[0] << "\n";
		stats.push_back({"C++_map_key_inc", 0.0, ms, (double)m[0]});
	}

	// --------------------------------------------------
	// 4. Zeus dict increment benchmark
	// --------------------------------------------------
	{
		std::string src;
		src += "var p = {};\n";
		src += "p[0] = 4;\n";
		src += "for (" + std::to_string(N) + ") {\n";
		src += "    p[0] = p[0] + 1;\n";
		src += "}\n";              // close loop
		if (keepDelayPrint) src += "delayPrint(p);\n";
		std::cout << "\n[Zeus] dict increment source begin\n";
		std::cout << src;
		std::cout << "[Zeus] dict increment source end\n";
		double totCompile = 0.0, totExec = 0.0;
		for (int r=0;r<runs;r++) {
			auto zr = runZeus(src, false, printIR && r==0);
			totCompile += zr.compileMs;
			totExec    += zr.execMs;
		}
		double avgC = totCompile/runs;
		double avgE = totExec/runs;
		std::cout << "[Zeus] dict inc avg compile(ms)=" << avgC
				  << " avg execute(ms)=" << avgE << "\n";
		stats.push_back({"Zeus_dict_inc", avgC, avgE, 0});
	}

	// -------------------- Summary Table --------------------
	std::cout << "\n==================== Summary ====================\n";
	// Find baselines for ratio: use C++ int increment as 1x if present
	double baseInt = 0.0;
	for (auto &e : stats) if (e.name == "C++_int") baseInt = e.execMs;
	if (baseInt <= 0) baseInt = 1.0; // avoid div0
	std::cout << "Name              Exec(ms)  Compile(ms)  x_vs_C++_int" << (printIR?"    Note":"") << "\n";
	for (auto &e : stats) {
		double ratio = e.execMs / baseInt;
		std::cout.setf(std::ios::fixed); std::cout.precision(3);
		std::cout << std::left;
		std::cout.width(18); std::cout << e.name;
		std::cout.width(10); std::cout << e.execMs;
		std::cout.width(13); std::cout << e.compileMs;
		std::cout.width(12); std::cout << ratio;
		if (e.name.rfind("Zeus_",0)==0 && printIR) std::cout << "  (IR shown first run)";
		std::cout << "\n";
	}
	std::cout << "================================================\n";

	if (keepDelayPrint) {
		std::cout << "Delayed prints:" << std::endl;
		// Use builtins printer (always writes to stdout) instead of ZeusCompile::printAll (debug gated)
		ZeusBuiltins::printAll();
	} else {
		std::cout << "(delayPrint skipped)" << std::endl;
	}
	return 0;
}

