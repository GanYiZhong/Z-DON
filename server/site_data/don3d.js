// Z-DON site: the Don's 3D look on my page, drawn from the published skin's Models/*.glb the way the game does
// (src/objects/global/chara_3d.cpp): a costume (cos/N.glb) or a head over a body (head/N.glb, body/N.glb), the
// taikoEffectChangeColors materials recoloured with the player's three colours (red-ish texels -> body, green ->
// face, blue -> rim), the face plane showing a frame of Models/face/face_000000.png, and the black outline whose
// per-vertex width is COLOR_0's green channel. It plays an idle loop of animations.glb, sent as a small JSON by
// /cosm/glb/anim/<name>.json (zdon_don_anim.py): without a pose the arms stick out and head and body pieces sit
// where nobody sees them. Bones as raylib does (the game's loader): bone = animated world * inverse(the part's own
// rest world), so no animation draws the meshes as stored. Plain WebGL 2, no libraries.
// Use: Don3D.attach(canvas, look), later .set(look); look = {mode: "costume"|"parts", costume, head, body,
// puchi (0: none), colors: ["#rrggbb" x3]}.
(function () {
  "use strict";
  var BASE = "/cosm/glb/";
  var ANIM = "don_normal";
  var FACE_PLANE = 0.137;
  var MAX_BONES = 64;
  // the puchichara (Models/acce/N.png: two frames stacked), placed as draw_puchi places it: 90 px left of and 120 px
  // above the model's origin, 150 px across, the frame flipping every 850 ms and bobbing 6 px; the skin draws the
  // model at 650 px a unit (skin_config chara_3d.scale), so those are model units / 650
  var PUCHI = "/cosm/puchichara/";
  var PUCHI_X = -90 / 650, PUCHI_Y = 120 / 650, PUCHI_SIZE = 150 / 650, PUCHI_BOB = 6 / 650, PUCHI_MS = 850;
  var cache = {};

  function cached(url, how) {
    if (!cache[url]) {
      cache[url] = fetch(url).then(function (r) {
        if (!r.ok) throw new Error(url + " " + r.status);
        return how(r);
      });
      cache[url].catch(function () { delete cache[url]; });
    }
    return cache[url];
  }
  function fetchGlb(url) { return cached(url, function (r) { return r.arrayBuffer().then(parseGlb); }); }
  function fetchImage(url) { return cached(url, function (r) { return r.blob().then(function (b) { return createImageBitmap(b); }); }); }
  function fetchAnim(url) { return cached(url, function (r) { return r.json().then(makeAnim); }); }

  // ---------------------------------------------------------------------------------------------------------------
  // matrices (column-major 4x4)

  function mul(a, b) {
    var o = new Float32Array(16);
    for (var c = 0; c < 4; c++)
      for (var r = 0; r < 4; r++)
        o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    return o;
  }

  function compose(t, q, s) {
    var x = q[0], y = q[1], z = q[2], w = q[3];
    return new Float32Array([
      (1 - 2 * (y * y + z * z)) * s[0], (2 * (x * y + z * w)) * s[0], (2 * (x * z - y * w)) * s[0], 0,
      (2 * (x * y - z * w)) * s[1], (1 - 2 * (x * x + z * z)) * s[1], (2 * (y * z + x * w)) * s[1], 0,
      (2 * (x * z + y * w)) * s[2], (2 * (y * z - x * w)) * s[2], (1 - 2 * (x * x + y * y)) * s[2], 0,
      t[0], t[1], t[2], 1]);
  }

  function trs(n) {
    return n.matrix ? new Float32Array(n.matrix) : compose(n.translation || [0, 0, 0], n.rotation || [0, 0, 0, 1], n.scale || [1, 1, 1]);
  }

  function invert(m) {
    var a00 = m[0], a01 = m[1], a02 = m[2], a03 = m[3], a10 = m[4], a11 = m[5], a12 = m[6], a13 = m[7],
        a20 = m[8], a21 = m[9], a22 = m[10], a23 = m[11], a30 = m[12], a31 = m[13], a32 = m[14], a33 = m[15];
    var b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10, b03 = a01 * a12 - a02 * a11,
        b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12, b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30,
        b08 = a20 * a33 - a23 * a30, b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
    var det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    if (!det) return new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);
    det = 1 / det;
    return new Float32Array([
      (a11 * b11 - a12 * b10 + a13 * b09) * det, (a02 * b10 - a01 * b11 - a03 * b09) * det,
      (a31 * b05 - a32 * b04 + a33 * b03) * det, (a22 * b04 - a21 * b05 - a23 * b03) * det,
      (a12 * b08 - a10 * b11 - a13 * b07) * det, (a00 * b11 - a02 * b08 + a03 * b07) * det,
      (a32 * b02 - a30 * b05 - a33 * b01) * det, (a20 * b05 - a22 * b02 + a23 * b01) * det,
      (a10 * b10 - a11 * b08 + a13 * b06) * det, (a01 * b08 - a00 * b10 - a03 * b06) * det,
      (a30 * b04 - a31 * b02 + a33 * b00) * det, (a21 * b02 - a20 * b04 - a23 * b00) * det,
      (a11 * b07 - a10 * b09 - a12 * b06) * det, (a00 * b09 - a01 * b07 + a02 * b06) * det,
      (a31 * b01 - a30 * b03 - a32 * b00) * det, (a20 * b03 - a21 * b01 + a22 * b00) * det]);
  }

  // world matrices of every node, given each node's local matrix and parent (-1: none)
  function worldsOf(locals, parents) {
    var out = new Array(locals.length);
    function get(i) {
      if (!out[i]) out[i] = parents[i] < 0 ? locals[i] : mul(get(parents[i]), locals[i]);
      return out[i];
    }
    for (var i = 0; i < locals.length; i++) get(i);
    return out;
  }

  // ---------------------------------------------------------------------------------------------------------------
  // the animation: rest skeleton + LINEAR tracks; pose(t) -> {name: world matrix}, face(t) -> expression index

  function makeAnim(doc) {
    var nodes = doc.nodes, parents = nodes.map(function (n) { return n.parent; });
    var extra = nodes.findIndex(function (n) { return n.name === "EXTRA"; });
    function sample(tr, t) {
      var ts = tr.times, n = tr.path === "rotation" ? 4 : 3, k = 0;
      while (k + 1 < ts.length && ts[k + 1] <= t) k++;
      var a = tr.values.slice(k * n, k * n + n);
      if (k + 1 >= ts.length || ts[k + 1] <= ts[k]) return a;
      var b = tr.values.slice((k + 1) * n, (k + 1) * n + n), f = Math.min(Math.max((t - ts[k]) / (ts[k + 1] - ts[k]), 0), 1);
      if (n === 4) { // nlerp on the short arc
        var d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], sg = d < 0 ? -1 : 1, o = [], l = 0;
        for (var c = 0; c < 4; c++) { o[c] = a[c] + (sg * b[c] - a[c]) * f; l += o[c] * o[c]; }
        l = Math.sqrt(l) || 1;
        return o.map(function (v) { return v / l; });
      }
      return a.map(function (v, c) { return v + (b[c] - v) * f; });
    }
    return {
      duration: doc.duration || 1,
      pose: function (t) {
        var T = nodes.map(function (n) { return n.t; }), R = nodes.map(function (n) { return n.r; }), S = nodes.map(function (n) { return n.s; });
        doc.tracks.forEach(function (tr) {
          var v = sample(tr, t);
          (tr.path === "translation" ? T : tr.path === "rotation" ? R : S)[tr.node] = v;
        });
        var W = worldsOf(nodes.map(function (_, i) { return compose(T[i], R[i], S[i]); }), parents), out = {};
        nodes.forEach(function (n, i) { out[n.name] = W[i]; });
        return out;
      },
      face: function (t) { // chara_3d.cpp read_glb_faces: EXTRA's translation x * 100
        if (extra < 0) return 0;
        var tr = doc.tracks.find(function (x) { return x.node === extra && x.path === "translation"; });
        var x = tr ? sample(tr, t)[0] : nodes[extra].t[0];
        return Math.min(Math.max(Math.round(x * 100), 0), 11);
      }
    };
  }

  // ---------------------------------------------------------------------------------------------------------------
  // glb -> parts

  var COMPONENTS = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT4: 16 };
  var SIZES = { 5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4 };
  var GETTERS = { 5120: "getInt8", 5121: "getUint8", 5122: "getInt16", 5123: "getUint16", 5125: "getUint32", 5126: "getFloat32" };
  var NORM = { 5120: 127, 5121: 255, 5122: 32767, 5123: 65535 };

  function readAccessor(doc, bin, i, asFloat) {
    var a = doc.accessors[i], v = doc.bufferViews[a.bufferView], n = COMPONENTS[a.type], size = SIZES[a.componentType];
    var stride = v.byteStride || n * size, off = (v.byteOffset || 0) + (a.byteOffset || 0);
    var out = asFloat ? new Float32Array(a.count * n) : new Uint32Array(a.count * n);
    var scale = asFloat && a.normalized && NORM[a.componentType] ? 1 / NORM[a.componentType] : 1;
    var view = new DataView(bin.buffer, bin.byteOffset, bin.byteLength), get = GETTERS[a.componentType];
    for (var k = 0; k < a.count; k++)
      for (var c = 0; c < n; c++) out[k * n + c] = view[get](off + k * stride + c * size, true) * scale;
    return { data: out, n: n, count: a.count };
  }

  function parseGlb(buf) {
    var dv = new DataView(buf);
    if (dv.getUint32(0, true) !== 0x46546c67) throw new Error("not a glb");
    var jsonLen = dv.getUint32(12, true);
    var doc = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, 20, jsonLen)));
    var binOff = 20 + jsonLen;
    return { doc: doc, bin: new Uint8Array(buf, binOff + 8, dv.getUint32(binOff, true)) };
  }

  function materialKind(m) {
    var name = (m.name || "").toLowerCase(), shader = (m.extras || {}).shaderType || "";
    if (shader === "taikoEffectFace" || name.indexOf("body_027000_02_") === 0) return "face";
    if (name.indexOf("_aa_add") >= 0) return "add";
    var cutout = name.indexOf("_color_s_cus_") >= 0 && name.indexOf("_a_ab") < 0;
    if (name.indexOf("_a_ab") >= 0 || (m.alphaMode === "BLEND" && !cutout)) return "blend";
    return "opaque";
  }

  function glbParts(g, isHead) {
    var doc = g.doc, bin = g.bin, parts = [];
    var parents = doc.nodes.map(function () { return -1; });
    doc.nodes.forEach(function (n, i) { (n.children || []).forEach(function (c) { parents[c] = i; }); });
    var W = worldsOf(doc.nodes.map(trs), parents);
    var images = (doc.images || []).map(function (im) {
      var v = doc.bufferViews[im.bufferView];
      return new Blob([bin.subarray(v.byteOffset || 0, (v.byteOffset || 0) + v.byteLength)], { type: im.mimeType || "image/png" });
    });
    doc.nodes.forEach(function (node, ni) {
      if (node.mesh === undefined) return;
      var skin = null;
      if (node.skin !== undefined) {
        var js = doc.skins[node.skin].joints.slice(0, MAX_BONES);
        skin = { names: js.map(function (j) { return doc.nodes[j].name || ""; }), restInv: js.map(function (j) { return invert(W[j]); }) };
      }
      doc.meshes[node.mesh].primitives.forEach(function (p) {
        if (p.mode !== undefined && p.mode !== 4) return;
        var at = p.attributes;
        var pos = readAccessor(doc, bin, at.POSITION, true).data;
        var count = pos.length / 3;
        var nrm = at.NORMAL !== undefined ? readAccessor(doc, bin, at.NORMAL, true).data : new Float32Array(pos.length);
        var uv = at.TEXCOORD_0 !== undefined ? readAccessor(doc, bin, at.TEXCOORD_0, true).data : new Float32Array(count * 2);
        var col = at.COLOR_0 !== undefined ? readAccessor(doc, bin, at.COLOR_0, true) : null;
        var m = doc.materials[p.material] || {}, kind = materialKind(m), i, v, c;
        if (kind === "face") { // chara_3d.cpp load_part: the face plane to the costume's face size
          var mini = (m.name || "").toLowerCase().indexOf("body_027000_02_") === 0;
          var mn = [1e9, 1e9, 1e9], mx = [-1e9, -1e9, -1e9];
          for (v = 0; v < count; v++) for (i = 0; i < 3; i++) { mn[i] = Math.min(mn[i], pos[v * 3 + i]); mx[i] = Math.max(mx[i], pos[v * 3 + i]); }
          var size = Math.max(mx[0] - mn[0], mx[1] - mn[1]);
          var plate = isHead || size > FACE_PLANE * 1.02 ? FACE_PLANE : size;
          if (size > 0.0001) {
            var f = (mini ? plate : plate * 4 / 3) / size;
            for (v = 0; v < count; v++) for (i = 0; i < 3; i++) {
              c = (mn[i] + mx[i]) / 2;
              pos[v * 3 + i] = c + (pos[v * 3 + i] - c) * f;
            }
          }
        }
        var J = new Float32Array(count * 4), Wt = new Float32Array(count * 4);
        if (skin && at.JOINTS_0 !== undefined && at.WEIGHTS_0 !== undefined) {
          J = readAccessor(doc, bin, at.JOINTS_0, true).data;
          Wt = readAccessor(doc, bin, at.WEIGHTS_0, true).data;
          for (i = 0; i < J.length; i++) if (J[i] >= skin.names.length) { J[i] = 0; Wt[i] = 0; }
        } else if (!skin) { // a plain mesh: its node's transform, once
          var M = W[ni];
          for (v = 0; v < count; v++) {
            var x = pos[v * 3], y = pos[v * 3 + 1], z = pos[v * 3 + 2];
            for (i = 0; i < 3; i++) pos[v * 3 + i] = M[i] * x + M[4 + i] * y + M[8 + i] * z + M[12 + i];
          }
        }
        var outline = new Float32Array(count);
        if (col && kind !== "face") for (v = 0; v < count; v++) outline[v] = col.data[v * col.n + 1];
        var idx = p.indices !== undefined ? readAccessor(doc, bin, p.indices, false).data : null;
        if (!idx) { idx = new Uint32Array(count); for (v = 0; v < count; v++) idx[v] = v; }
        var tex = m.pbrMetallicRoughness && m.pbrMetallicRoughness.baseColorTexture;
        parts.push({
          pos: pos, nrm: nrm, uv: uv, outline: outline, joints: J, weights: Wt, idx: idx, kind: kind,
          image: tex ? images[doc.textures[tex.index].source] : null, skin: skin,
          recolor: (m.extras || {}).shaderType === "taikoEffectChangeColors" && kind !== "face",
          twoSided: (m.name || "").toLowerCase().indexOf("cullnone") >= 0 || !!m.doubleSided
        });
      });
    });
    return parts;
  }

  // bone matrices of a part for a pose ({name: world} or null: the rest pose)
  function boneMatrices(skin, pose) {
    var out = new Float32Array(MAX_BONES * 16);
    for (var k = 0; k < MAX_BONES; k++) out[k * 16] = out[k * 16 + 5] = out[k * 16 + 10] = out[k * 16 + 15] = 1;
    if (!skin || !pose) return out;
    skin.names.forEach(function (name, k) {
      if (pose[name]) out.set(mul(pose[name], skin.restInv[k]), k * 16);
    });
    return out;
  }

  // ---------------------------------------------------------------------------------------------------------------
  // drawing

  var VS = "#version 300 es\nin vec3 p;in vec3 n;in vec2 t;in float o;in vec4 j;in vec4 w;uniform mat4 bones[" + MAX_BONES + "];" +
    "uniform mat4 mvp;uniform mat3 rot;uniform vec2 px;uniform float outlinePass;out vec2 uv;void main(){" +
    "vec4 q=vec4(p,1.0);vec3 m=n;float s=w.x+w.y+w.z+w.w;if(s>0.0){mat4 b=(w.x*bones[int(j.x)]+w.y*bones[int(j.y)]+" +
    "w.z*bones[int(j.z)]+w.w*bones[int(j.w)])/s;q=b*q;m=mat3(b)*n;}vec4 c=mvp*q;" +
    "if(outlinePass>0.5){vec3 e=rot*m;if(dot(e.xy,e.xy)>1e-12){c.xy+=px*o*normalize(e.xy)*c.w;}}gl_Position=c;uv=t;}";
  var FS = "#version 300 es\nprecision mediump float;in vec2 uv;uniform sampler2D tex;uniform int mode;uniform vec3 c1;" +
    "uniform vec3 c2;uniform vec3 c3;uniform float faceRows;uniform float faceIndex;out vec4 o;void main(){gl_FragDepth=gl_FragCoord.z;" +
    "if(mode==3){o=vec4(0.0,0.0,0.0,1.0);return;}" +
    "vec2 st=uv;if(mode==2){st=vec2(uv.x,(clamp(uv.y,0.0,1.0)+faceIndex)/faceRows);}vec4 x=texture(tex,st);" +
    "if(mode==2){if(x.a<0.01)discard;gl_FragDepth=gl_FragCoord.z-0.001;o=x;return;}" +
    "if(x.a<=0.0)discard;if(mode==1){float s=max(x.r,max(x.g,x.b)),w=min(x.r,min(x.g,x.b));" +
    "if(s>0.05&&(s-w)>0.08){if(x.b>x.r&&x.b>=x.g)x.rgb=c3;else if(x.g>x.r&&x.g>x.b)x.rgb=c2;else x.rgb=c1;}}o=x;}";
  var ATTRS = ["p", "n", "t", "o", "j", "w"];
  var UNIFORMS = ["mvp", "rot", "px", "outlinePass", "tex", "mode", "c1", "c2", "c3", "faceRows", "faceIndex", "bones"];

  function hex(c) {
    var m = /^#?([0-9a-f]{6})$/i.exec(c || "") || [0, "ffffff"];
    var v = parseInt(m[1], 16);
    return [(v >> 16 & 255) / 255, (v >> 8 & 255) / 255, (v & 255) / 255];
  }

  function Viewer(canvas) {
    var gl = canvas.getContext("webgl2", { antialias: true, alpha: true, premultipliedAlpha: false });
    this.canvas = canvas;
    this.gl = gl;
    this.angle = 0;
    this.parts = [];
    this.look = null;
    this.serial = 0;
    this.anim = null;
    this.start = performance.now();
    if (!gl) return;
    var prog = gl.createProgram();
    [[gl.VERTEX_SHADER, VS], [gl.FRAGMENT_SHADER, FS]].forEach(function (s) {
      var sh = gl.createShader(s[0]);
      gl.shaderSource(sh, s[1]);
      gl.compileShader(sh);
      if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(sh));
      gl.attachShader(prog, sh);
    });
    ATTRS.forEach(function (a, i) { gl.bindAttribLocation(prog, i, a); });
    gl.linkProgram(prog);
    this.prog = prog;
    this.u = {};
    var self = this;
    UNIFORMS.forEach(function (n) { self.u[n] = gl.getUniformLocation(prog, n); });
    // drag to turn; until then it sways a little either side of facing front
    var drag = null;
    this.spin = true;
    canvas.style.touchAction = "none";
    canvas.addEventListener("pointerdown", function (e) { drag = e.clientX; self.spin = false; canvas.setPointerCapture(e.pointerId); });
    canvas.addEventListener("pointermove", function (e) {
      if (drag === null) return;
      self.angle += (e.clientX - drag) * 0.012;
      drag = e.clientX;
    });
    canvas.addEventListener("pointerup", function () { drag = null; });
    function tick(t) {
      if (self.parts.length && document.visibilityState === "visible") {
        if (self.spin) self.angle = 0.4 * Math.sin(t * 0.0006);
        self.draw(t);
      }
      requestAnimationFrame(tick);
    }
    requestAnimationFrame(tick);
    // the puchichara: a flat sprite over the canvas, as the game draws it (chara_3d.cpp draw_puchi)
    var holder = canvas.parentNode;
    if (holder && getComputedStyle(holder).position === "static") holder.style.position = "relative";
    this.puchiEl = document.createElement("div");
    this.puchiEl.style.cssText = "position:absolute;pointer-events:none;display:none;background-repeat:no-repeat;" +
      "background-size:100% 200%";
    if (holder) holder.appendChild(this.puchiEl);
    this.puchi = 0;
    fetchAnim(BASE + "anim/" + ANIM + ".json").then(function (a) {
      self.anim = a;
      if (self.parts.length) self.bounds();
    }).catch(function () { /* the rest pose, then */ });
  }

  Viewer.prototype.upload = function (part, bitmap) {
    var gl = this.gl;
    var vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    [[part.pos, 3], [part.nrm, 3], [part.uv, 2], [part.outline, 1], [part.joints, 4], [part.weights, 4]].forEach(function (a, i) {
      var b = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, b);
      gl.bufferData(gl.ARRAY_BUFFER, a[0], gl.STATIC_DRAW);
      gl.enableVertexAttribArray(i);
      gl.vertexAttribPointer(i, a[1], gl.FLOAT, false, 0, 0);
    });
    var ib = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, part.idx, gl.STATIC_DRAW);
    gl.bindVertexArray(null);
    var tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    if (bitmap) gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, bitmap);
    else gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([255, 255, 255, 255]));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, part.kind === "face" ? gl.CLAMP_TO_EDGE : gl.REPEAT);
    return { vao: vao, count: part.idx.length, tex: tex, kind: part.kind, recolor: part.recolor, twoSided: part.twoSided,
             skin: part.skin, src: part };
  };

  Viewer.prototype.set = function (look) {
    var self = this, gl = this.gl, serial = ++this.serial;
    this.look = look;
    if (!gl) return Promise.resolve();
    var puchi = +look.puchi || 0;
    if (puchi !== this.puchi) {
      this.puchi = puchi;
      var el = this.puchiEl;
      el.style.display = "none";
      if (puchi > 0) {
        var img = new Image(), url = PUCHI + puchi + ".png";
        img.onload = function () {
          if (self.puchi !== puchi) return;
          el.style.backgroundImage = "url(" + url + ")";
          el.style.display = "";
        };
        img.src = url;
      }
      if (this.parts.length) this.bounds();
    }
    var files = look.mode === "costume" ? [["cos", look.costume, false]] : [["body", look.body, false], ["head", look.head, true]];
    var key = JSON.stringify(files);
    if (key === this.key) return Promise.resolve();
    this.canvas.classList.add("loading");
    return Promise.all([fetchImage(BASE + "face.png")].concat(files.map(function (f) {
      return fetchGlb(BASE + f[0] + "/" + (f[1] || 0) + ".glb").then(function (g) { return glbParts(g, f[2]); });
    }))).then(function (got) {
      var face = got[0], parts = [].concat.apply([], got.slice(1));
      return Promise.all(parts.map(function (p) {
        return p.kind === "face" ? Promise.resolve(face) : p.image ? createImageBitmap(p.image) : Promise.resolve(null);
      })).then(function (bitmaps) {
        if (serial !== self.serial) return;
        self.drop();
        self.faceRows = face.height / face.width;
        self.parts = parts.map(function (p, i) { return self.upload(p, bitmaps[i]); });
        self.key = key;
        self.bounds();
        self.canvas.classList.remove("loading", "failed");
      });
    }).catch(function () {
      if (serial !== self.serial) return;
      self.canvas.classList.remove("loading");
      self.canvas.classList.add("failed");
      self.puchiEl.style.display = "none";
    });
  };

  Viewer.prototype.drop = function () {
    var gl = this.gl;
    this.parts.forEach(function (p) { gl.deleteVertexArray(p.vao); gl.deleteTexture(p.tex); });
    this.parts = [];
  };

  // the framing: the posed model over the loop (skinned on the CPU at a few moments), so it stays in view
  Viewer.prototype.bounds = function () {
    var mn = [1e9, 1e9, 1e9], mx = [-1e9, -1e9, -1e9], anim = this.anim, parts = this.parts;
    var times = anim ? [0, 0.25, 0.5, 0.75].map(function (f) { return f * anim.duration; }) : [0];
    times.forEach(function (t) {
      var pose = anim ? anim.pose(t) : null;
      parts.forEach(function (part) {
        var p = part.src;
        if (p.kind === "face") return;
        var B = boneMatrices(p.skin, pose), c;
        for (var v = 0; v < p.pos.length / 3; v++) {
          var x = p.pos[v * 3], y = p.pos[v * 3 + 1], z = p.pos[v * 3 + 2], q = [0, 0, 0], s = 0;
          for (var k = 0; k < 4; k++) {
            var w = p.weights[v * 4 + k];
            if (!w) continue;
            var b = p.joints[v * 4 + k] * 16;
            s += w;
            for (c = 0; c < 3; c++) q[c] += w * (B[b + c] * x + B[b + 4 + c] * y + B[b + 8 + c] * z + B[b + 12 + c]);
          }
          if (!s) q = [x, y, z];
          else for (c = 0; c < 3; c++) q[c] /= s;
          for (c = 0; c < 3; c++) { mn[c] = Math.min(mn[c], q[c]); mx[c] = Math.max(mx[c], q[c]); }
        }
      });
    });
    if (this.puchi > 0) { // the sprite's square (and its bob) stays in view too
      var h = PUCHI_SIZE / 2 + PUCHI_BOB;
      mn[0] = Math.min(mn[0], PUCHI_X - h); mx[0] = Math.max(mx[0], PUCHI_X + h);
      mn[1] = Math.min(mn[1], PUCHI_Y - h); mx[1] = Math.max(mx[1], PUCHI_Y + h);
    }
    this.center = [(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2];
    this.radius = Math.max(mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]) * 0.62 || 1;
  };

  Viewer.prototype.draw = function (now) {
    var gl = this.gl, c = this.canvas;
    if (!gl || !this.parts.length) return;
    var dpr = window.devicePixelRatio || 1, w = Math.round(c.clientWidth * dpr), h = Math.round(c.clientHeight * dpr);
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
    var anim = this.anim, t = anim ? (((now || performance.now()) - this.start) / 1000) % anim.duration : 0;
    var pose = anim ? anim.pose(t) : null;
    gl.viewport(0, 0, w, h);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.useProgram(this.prog);
    // orthographic, centred on the model, turned about the vertical axis; the Don faces +z (the viewer at angle 0)
    var r = this.radius, asp = w / h, sx = asp >= 1 ? r * asp : r, sy = asp >= 1 ? r : r / asp;
    var ca = Math.cos(this.angle), sa = Math.sin(this.angle), cx = this.center[0], cy = this.center[1], cz = this.center[2];
    gl.uniformMatrix4fv(this.u.mvp, false, new Float32Array([
      ca / sx, 0, sa * 0.5 / r, 0,
      0, 1 / sy, 0, 0,
      sa / sx, 0, -ca * 0.5 / r, 0,
      (-ca * cx - sa * cz) / sx, -cy / sy, (ca * cz - sa * cx) * 0.5 / r, 1]));
    gl.uniformMatrix3fv(this.u.rot, false, new Float32Array([ca, 0, -sa, 0, 1, 0, sa, 0, ca]));
    if (this.puchi > 0) { // screen space beside the model's origin, which turns with the model
      var cssW = c.clientWidth, cssH = c.clientHeight, ppu = cssH / (2 * sy);
      var ox = (ca * -cx + sa * -cz) / sx, oy = -cy / sy; // the origin in clip space
      var elapsed = (now || performance.now()) - this.start;
      var bob = Math.sin(elapsed / (PUCHI_MS * 2) * 2 * Math.PI) * PUCHI_BOB;
      var size = PUCHI_SIZE * ppu;
      var pcx = (ox + 1) / 2 * cssW + PUCHI_X * ppu, pcy = (1 - oy) / 2 * cssH - (PUCHI_Y + bob) * ppu;
      var st = this.puchiEl.style;
      st.width = st.height = size + "px";
      st.left = c.offsetLeft + pcx - size / 2 + "px";
      st.top = c.offsetTop + pcy - size / 2 + "px";
      st.backgroundPosition = "0 " + (Math.floor(elapsed / PUCHI_MS) % 2 ? "100%" : "0");
    }
    var line = 2.5 * h / 300; // the game's 2.5 px at 720p for a Don about this tall
    gl.uniform2f(this.u.px, line * 2 / w, line * 2 / h);
    var colors = (this.look && this.look.colors) || [];
    gl.uniform3fv(this.u.c1, hex(colors[0]));
    gl.uniform3fv(this.u.c2, hex(colors[1]));
    gl.uniform3fv(this.u.c3, hex(colors[2]));
    gl.uniform1f(this.u.faceRows, this.faceRows || 1);
    gl.uniform1f(this.u.faceIndex, anim ? Math.min(anim.face(t), (this.faceRows || 1) - 1) : 0);
    gl.uniform1i(this.u.tex, 0);
    gl.activeTexture(gl.TEXTURE0);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.frontFace(gl.CCW);
    var self = this, bones = new Map();
    function pass(filter, outline) {
      self.parts.forEach(function (p) {
        if (!filter(p)) return;
        if (!bones.has(p.skin)) bones.set(p.skin, boneMatrices(p.skin, pose));
        gl.uniformMatrix4fv(self.u.bones, false, bones.get(p.skin));
        gl.uniform1f(self.u.outlinePass, outline ? 1 : 0);
        gl.uniform1i(self.u.mode, outline ? 3 : p.kind === "face" ? 2 : p.recolor ? 1 : 0);
        if (outline) { gl.enable(gl.CULL_FACE); gl.cullFace(gl.FRONT); }
        else if (p.twoSided || p.kind === "face") gl.disable(gl.CULL_FACE);
        else { gl.enable(gl.CULL_FACE); gl.cullFace(gl.BACK); }
        gl.bindTexture(gl.TEXTURE_2D, p.tex);
        gl.bindVertexArray(p.vao);
        gl.drawElements(gl.TRIANGLES, p.count, gl.UNSIGNED_INT, 0);
      });
    }
    function is(kind) { return function (p) { return p.kind === kind; }; }
    gl.disable(gl.BLEND);
    gl.depthMask(true);
    pass(is("opaque"), true); // the outline hull first
    gl.enable(gl.BLEND);
    gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    pass(is("opaque"));
    pass(is("face"));
    gl.depthMask(false);
    pass(is("blend"));
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE);
    pass(is("add"));
    gl.depthMask(true);
    gl.bindVertexArray(null);
  };

  window.Don3D = {
    attach: function (canvas, look) {
      var v = new Viewer(canvas);
      if (!v.gl) { canvas.classList.add("failed"); return v; }
      v.set(look);
      return v;
    }
  };
})();
