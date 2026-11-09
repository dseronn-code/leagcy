// Retarget de clipes de mocap (Mixamo / Auto-Rig Pro) para o esqueleto Character Creator do jogador.
// Qt(t) = Qs(t) * inv(Rs) * inv(C) * Rt   (rotações de mundo; Rs/Rt = pose de referência; C alinha a direção do osso)
const ARP = { rootx: "Hips", spine_01x: "Spine", spine_02x: "Spine1", spine_03x: "Spine2", neckx: "Neck", headx: "Head" };
for (const [s, S] of [["l", "Left"], ["r", "Right"]]) {
  Object.assign(ARP, { ["shoulder" + s]: S + "Shoulder", ["arm_stretch" + s]: S + "Arm", ["forearm_stretch" + s]: S + "ForeArm", ["hand" + s]: S + "Hand",
    ["thigh_stretch" + s]: S + "UpLeg", ["leg_stretch" + s]: S + "Leg", ["foot" + s]: S + "Foot", ["toes_01" + s]: S + "ToeBase" });
  for (const [a, A] of [["thumb", "Thumb"], ["index", "Index"], ["middle", "Middle"], ["ring", "Ring"], ["pinky", "Pinky"]]) for (let k = 1; k <= 3; k++) ARP["c_" + a + k + s] = S + "Hand" + A + k;
}
const MIX = /(Hips|Spine2|Spine1|Spine|Neck|Head|(?:Left|Right)(?:Shoulder|ForeArm|Arm|Hand(?:Thumb|Index|Middle|Ring|Pinky)[1-3]|Hand|UpLeg|Leg|Foot|ToeBase))$/;
function canonSrc(name) {
  const n = name.replace(/_\d+$/, "");
  if (ARP[n]) return ARP[n];
  const m = n.match(MIX); return m ? m[1] : null;
}
const CC = { Hips: "Hip", Spine: "Waist", Spine1: "Spine01", Spine2: "Spine02", Neck: "NeckTwist01", Head: "Head" };
for (const [S, c] of [["Left", "L"], ["Right", "R"]]) {
  Object.assign(CC, { [S + "Shoulder"]: c + "_Clavicle", [S + "Arm"]: c + "_Upperarm", [S + "ForeArm"]: c + "_Forearm", [S + "Hand"]: c + "_Hand",
    [S + "UpLeg"]: c + "_Thigh", [S + "Leg"]: c + "_Calf", [S + "Foot"]: c + "_Foot", [S + "ToeBase"]: c + "_ToeBase" });
  for (const [A, B] of [["Thumb", "Thumb"], ["Index", "Index"], ["Middle", "Mid"], ["Ring", "Ring"], ["Pinky", "Pinky"]]) for (let k = 1; k <= 3; k++) CC[S + "Hand" + A + k] = c + "_" + B + k;
}
const CHILD = { Hips: "Spine", Spine: "Spine1", Spine1: "Spine2", Spine2: "Neck", Neck: "Head" };
for (const S of ["Left", "Right"]) {
  Object.assign(CHILD, { [S + "Shoulder"]: S + "Arm", [S + "Arm"]: S + "ForeArm", [S + "ForeArm"]: S + "Hand", [S + "Hand"]: S + "HandMiddle1", [S + "UpLeg"]: S + "Leg", [S + "Leg"]: S + "Foot", [S + "Foot"]: S + "ToeBase" });
  for (const A of ["Thumb", "Index", "Middle", "Ring", "Pinky"]) for (let k = 1; k < 3; k++) CHILD[S + "Hand" + A + k] = S + "Hand" + A + (k + 1);
}
const PARENT_C = { Head: "Neck" };
for (const S of ["Left", "Right"]) { PARENT_C[S + "ToeBase"] = S + "Foot"; for (const A of ["Thumb", "Index", "Middle", "Ring", "Pinky"]) PARENT_C[S + "Hand" + A + 3] = S + "Hand" + A + 2; }
const swapSide = (c) => c.startsWith("Left") ? "Right" + c.slice(4) : c.startsWith("Right") ? "Left" + c.slice(5) : c;

function collect(root, canon) {
  const B = {};
  root.traverse((o) => { if (!o.isBone && !o.isObject3D) return; if (!o.isBone) return; const c = canon(o.name); if (c && !B[c]) B[c] = o; });
  return B;
}
const canonTgt = (name) => { if (/scaleCompensation|Twist|Share/.test(name)) return null; const n = name.replace(/^CC_Base_/, "").replace(/_\d+$/, ""); for (const k in CC) if (CC[k] === n) return k; return null; };

// snapshot de mundo (pos, quat) de todos os ossos canônicos; opcionalmente corrigido de direção e espelhado
function snapshot(B, yaw, mirror) {
  const out = {}, Y = new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0, 1, 0), yaw);
  for (const c in B) {
    const o = B[c], p = o.getWorldPosition(new THREE.Vector3()), q = o.getWorldQuaternion(new THREE.Quaternion());
    p.applyQuaternion(Y); q.premultiply(Y);
    const key = mirror ? swapSide(c) : c;
    if (mirror) { p.x = -p.x; q.set(q.x, -q.y, -q.z, q.w); }
    out[key] = { p, q };
  }
  return out;
}
function facingYaw(B) {
  const l = B.LeftUpLeg.getWorldPosition(new THREE.Vector3()).sub(B.RightUpLeg.getWorldPosition(new THREE.Vector3()));
  // de frente para +z: esquerda = +x
  return -Math.atan2(-l.z, l.x);
}
function dirC(snapS, snapT, c) {
  // rotação que leva a direção de referência do osso na fonte até a do alvo
  let k = c; while (!CHILD[k] && PARENT_C[k]) k = PARENT_C[k];
  const ch = CHILD[k];
  if (!ch || !snapS[k] || !snapS[ch] || !snapT[k] || !snapT[ch]) return new THREE.Quaternion();
  const ds = snapS[ch].p.clone().sub(snapS[k].p).normalize(), dt = snapT[ch].p.clone().sub(snapT[k].p).normalize();
  return new THREE.Quaternion().setFromUnitVectors(ds, dt);
}

// src: {scene, animations}; tgt: {root (contêiner com o modelo), B (ossos CC canônicos)}
function retarget(src, tgt, opt) {
  const clip = src.animations[0];
  const SB = collect(src.scene, canonSrc);
  if (!tgt.rest) { tgt.rest = new Map(); tgt.root.traverse((o) => { if (o.isBone) tgt.rest.set(o, [o.position.clone(), o.quaternion.clone()]); }); }
  for (const [o, [p, q]] of tgt.rest) { o.position.copy(p); o.quaternion.copy(q); }
  src.scene.updateMatrixWorld(true);
  const yaw = facingYaw(SB);
  const refS = snapshot(SB, yaw, opt.mirror);
  tgt.root.updateMatrixWorld(true);
  const refT = snapshot(tgt.B, 0, false);
  const order = []; tgt.root.traverse((o) => { const c = canonTgt(o.name); if (o.isBone && c && refS[c]) order.push([c, o]); });
  const OFF = {};
  for (const [c] of order) { const C = dirC(refS, refT, c); OFF[c] = refS[c].q.clone().invert().multiply(C.invert()).multiply(refT[c].q); }
  const ground = (R) => Math.min(...["LeftFoot", "RightFoot", "LeftToeBase", "RightToeBase"].filter((c) => R[c]).map((c) => R[c].p.y));
  const leg = (R) => R.LeftUpLeg.p.distanceTo(R.LeftLeg.p) + R.LeftLeg.p.distanceTo(R.LeftFoot.p);
  const hipScale = leg(refT) / leg(refS);
  const gT = ground(refT);
  const mx0 = new THREE.AnimationMixer(src.scene); mx0.clipAction(clip).play(); mx0.setTime(opt.start || 0); src.scene.updateMatrixWorld(true);
  const F0 = snapshot(SB, yaw, opt.mirror), gS = ground(F0); mx0.stopAllAction(); mx0.uncacheRoot(src.scene);
  const restLocal = new Map(); tgt.root.traverse((o) => { if (o.isBone) restLocal.set(o, o.quaternion.clone()); });
  const mixer = new THREE.AnimationMixer(src.scene); const act = mixer.clipAction(clip); act.play();
  const fps = opt.fps || 30, t0 = opt.start || 0, t1 = Math.min(opt.end || clip.duration, clip.duration);
  const times = [], Q = {}, P = [];
  for (const [c] of order) Q[c] = [];
  const meta = { frames: [] };
  const hip = tgt.B.Hips;
  for (let t = t0, f = 0; t <= t1 + 1e-6; t = t0 + (++f) / fps) {
    mixer.setTime(t); src.scene.updateMatrixWorld(true);
    const S = snapshot(SB, yaw, opt.mirror);
    for (const [o, q] of restLocal) o.quaternion.copy(q);
    // quadril (posição): relativa à referência, escalada
    const hp = S.Hips.p.clone().sub(new THREE.Vector3(F0.Hips.p.x, gS, F0.Hips.p.z)).multiplyScalar(hipScale).add(new THREE.Vector3(refT.Hips.p.x, gT, refT.Hips.p.z));
    if (opt.rootScale) { hp.x = refT.Hips.p.x + (hp.x - refT.Hips.p.x) * opt.rootScale[0]; hp.z = refT.Hips.p.z + (hp.z - refT.Hips.p.z) * opt.rootScale[1]; }
    // deslocamento no campo sai do clipe (o jogo move o contêiner); o quadril fica sobre a origem
    const rootXZ = [hp.x - refT.Hips.p.x, hp.z - refT.Hips.p.z];
    if (opt.lockXZ) { rootXZ[0] = 0; rootXZ[1] = 0; }
    hp.x = refT.Hips.p.x; hp.z = refT.Hips.p.z;
    hip.parent.updateWorldMatrix(true, false);
    const hl = hip.parent.worldToLocal(hp.clone());
    hip.position.copy(hl);
    for (const [c, o] of order) {
      const qt = S[c].q.clone().multiply(OFF[c]);
      o.parent.updateWorldMatrix(true, false);
      const pq = o.parent.getWorldQuaternion(new THREE.Quaternion()).invert();
      o.quaternion.copy(pq.multiply(qt));
      o.updateMatrixWorld(true);
    }
    // pés no chão: o pé mais baixo do alvo fica na mesma altura (escalada) do pé mais baixo da fonte
    tgt.root.updateMatrixWorld(true);
    const want = (ground(S) - gS) * hipScale + gT;
    const have = Math.min(...["LeftFoot", "RightFoot", "LeftToeBase", "RightToeBase"].map((c) => tgt.B[c].getWorldPosition(new THREE.Vector3()).y));
    const hw = hip.getWorldPosition(new THREE.Vector3()); hw.y += want - have;
    hip.position.copy(hip.parent.worldToLocal(hw)); tgt.root.updateMatrixWorld(true);
    times.push(+(t - t0).toFixed(4));
    for (const [c, o] of order) Q[c].push(...o.quaternion.toArray().map((v) => +v.toFixed(4)));
    P.push(...hip.position.toArray().map((v) => +v.toFixed(4)));
    tgt.root.updateMatrixWorld(true);
    const w = (c) => tgt.B[c].getWorldPosition(new THREE.Vector3()).toArray().map((v) => +v.toFixed(3));
    meta.frames.push({ t: +(t - t0).toFixed(4), root: rootXZ.map((v) => +v.toFixed(3)), hips: w("Hips"), lf: w("LeftFoot"), rf: w("RightFoot"), lt: w("LeftToeBase"), rt: w("RightToeBase"), lh: w("LeftHand"), rh: w("RightHand"), head: w("Head") });
  }
  for (const [o, [p, q]] of tgt.rest) { o.position.copy(p); o.quaternion.copy(q); }
  const tracks = [];
  for (const [c, o] of order) tracks.push(new THREE.QuaternionKeyframeTrack(o.name + ".quaternion", times, Q[c]));
  tracks.push(new THREE.VectorKeyframeTrack(hip.name + ".position", times, P));
  const out = new THREE.AnimationClip(opt.name, times[times.length - 1], tracks);
  return { clip: out, meta };
}
