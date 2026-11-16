(* verify_numbers.wls
   复算文档中出现的每一个数，并逐条对照断言。
   任何一条超出容差即以退出码 1 结束，便于接入 CI。

   运行：wolframscript -file verify_numbers.wls
   需要：Wolfram Engine 12.0+（免费开发者许可即可）

   脚本只做复算，不读 raw.csv；输入参数与固件 / analyze.py 保持一致：
     sigma = 2.5 dB  多径引起的单点标准差（典型值，未实测）
     N     = 50      board b 的 APP_MEAS_N_DEFAULT
*)

pass = 0; fail = 0;

check[label_, computed_, expected_, tol_] := Module[{ok, shown},
  ok = Abs[computed - expected] <= tol;
  If[ok, pass++, fail++];
  shown = ToString[NumberForm[N[computed, 7], {9, 4}]];
  Print[If[ok, "  OK   ", "  FAIL "],
        StringPadRight[label, 38],
        " got ", StringPadRight[shown, 10],
        " expect ", expected, " +/- ", tol]
];

(* ---------- 输入参数 ---------- *)
sigma = 2.5;
nDef = 50;
sQ = 0.5/Sqrt[3];
f = 2.4*10^9;
c0 = 2.99792458*10^8;
mu0 = 4*Pi*10^-7;

(* ---------- uncertainty.md: 不确定度链 ---------- *)
Print["[uncertainty] error chain"];
sMed = 1.2533*sigma/Sqrt[nDef];
check["sigma_med = 1.2533 sigma/sqrt(N)", sMed, 0.44, 0.005];
check["sigma_q = 0.5/sqrt(3)", sQ, 0.29, 0.005];
sPoint = Sqrt[sMed^2 + sQ^2];
check["sigma_point", sPoint, 0.53, 0.005];
sL = Sqrt[2]*sPoint;
check["sigma_L", sL, 0.75, 0.005];
check["Delta_min (power 0.80)", 2.80*sL, 2.1, 0.02];
check["Delta_min (power 0.95)", 3.61*sL, 2.7, 0.02];

(* ---------- uncertainty.md: 量化地板 ---------- *)
Print["[uncertainty] quantization floor"];
check["floor as N->inf", 2.80*Sqrt[2]*sQ, 1.14, 0.005];

(* ---------- uncertainty.md: 采样量分配 ---------- *)
Print["[uncertainty] sample allocation"];
cst = 2.80*1.2533*Sqrt[2];
check["constant c = 2.80*1.2533*sqrt(2)", cst, 4.96, 0.005];
nNoQ = (cst*sigma/2.1)^2;
check["N for delta=2.1 (no quant)", nNoQ, 35, 0.5];
delta[nn_] := 2.80*Sqrt[2]*Sqrt[(1.2533*sigma/Sqrt[nn])^2 + sQ^2];
nQ = nn /. FindRoot[delta[nn] == 2.1, {nn, 40}];
check["N for delta=2.1 (with quant)", nQ, 50, 0.5];
check["sample penalty of quantization %", (nQ/nNoQ - 1)*100, 43, 1];

(* ---------- uncertainty.md: 中位数效率 ---------- *)
Print["[uncertainty] median efficiency"];
check["2/Pi", 2/Pi, 0.637, 0.001];
check["N equivalent for mean-level precision", nDef*Pi/2, 79, 0.5];

(* ---------- measurement-principle.md: 物理量 ---------- *)
Print["[principle] physical quantities"];
lam = c0/f;
check["lambda (cm)", lam*100, 12.5, 0.02];
check["lambda/2Pi (cm)", lam/(2*Pi)*100, 1.99, 0.005];
check["lambda/4 (cm)", lam/4*100, 3.12, 0.005];
fspl[d_] := 20*Log10[4*Pi*d/lam];
check["FSPL(0.25 m) dB", fspl[0.25], 28.0, 0.02];
check["FSPL(1.00 m) dB", fspl[1.00], 40.1, 0.05];
check["FSPL difference dB", fspl[1.00] - fspl[0.25], 12.0, 0.05];
check["Fresnel r1 at 0.25 m (cm)", Sqrt[lam*0.125*0.125/0.25]*100, 8.8, 0.05];
check["2 r1 (cm)", 2*Sqrt[lam*0.125*0.125/0.25]*100, 17.7, 0.05];
check["skin depth Cu (um)", Sqrt[2/(2*Pi*f*mu0*5.8*10^7)]*10^6, 1.35, 0.005];
check["skin depth Al (um)", Sqrt[2/(2*Pi*f*mu0*3.5*10^7)]*10^6, 1.74, 0.005];

(* ---------- 介质损耗量级 ---------- *)
Print["[principle] dielectric loss"];
aLow[e_, t_] := (2*Pi*f/c0)*Sqrt[e]*t/2;
check["L(25mm, eps=1.2, tan=.05) dB", 8.686*aLow[1.2, 0.05]*0.025, 0.30, 0.02];
check["L(125mm, eps=1.5, tan=.05) dB", 8.686*aLow[1.5, 0.05]*0.125, 1.67, 0.02];

Print[""];
Print["---- ", pass, " passed, ", fail, " failed ----"];
If[fail > 0, Exit[1]];
