import sys
for line in open(sys.argv[1]):
    f = line.strip().split(",")
    kv = dict(x.split("=", 1) for x in f[5:] if "=" in x)
    if line.startswith("MODULES"):
        print(f"M {f[2]:12s} {f[3]:>6s} {f[4]:>4s} avg={kv['total_avg%']:>6s} worst={kv['total_worst%']:>7s} "
              f"struct={kv['total_structWorst%']:>7s} clay_avg={kv['clay_avg%']:>6s} clay_worst={kv['clay_worst%']:>7s} lat={kv['latency']}")
    elif line.startswith("XOVER"):
        keys = ["period_ms","events","switch_phases","switch_clay_med_us","switch_clay_worst_us","other_clay_med_us","switch_extra_us","switch_extra%",
                "switch_block_med%","other_block_med%","switch_block_worst%","other_block_worst%",
                "switch_block_structWorst%","other_block_structWorst%","worst_increase_pp"]
        print(f"X {f[3]:>6s} {f[4]:>4s} " + " ".join(f"{k}={kv[k]}" for k in keys))
