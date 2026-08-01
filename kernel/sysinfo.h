struct sysinfo {
  uint64 freemem;   // amount of free memory (bytes)
  uint64 nproc;     // number of process
  int load_avg;     // 负载平均值，实际值乘以 10（如 50 代表 5.0）
};
