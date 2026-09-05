#ifdef _WIN32
extern "C" __declspec(dllexport) int unrelated_export() { return 1; }
#else
extern "C" __attribute__((visibility("default"))) int unrelated_export() { return 1; }
#endif
