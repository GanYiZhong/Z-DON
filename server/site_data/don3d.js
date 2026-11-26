// Z-DON site: the Don's 3D look on my page, drawn from the published skin's Models/*.glb the way the game does
// (src/objects/global/chara_3d.cpp): a costume (cos/N.glb) or a head over a body (head/N.glb, body/N.glb), the
// taikoEffectChangeColors materials recoloured with the player's three colours (red-ish texels -> body, green ->
// face, blue -> rim), the face plane showing the first frame of Models/face/face_000000.png, and the black outline
// whose per-vertex width is COLOR_0's green channel. No animation: the bind pose, as the meshes are stored.
// Plain WebGL 2, no libraries. Use: Don3D.attach(canvas, look) and later .set(look) with
// look = {mode: "costume"|"parts", costume, head, body, colors: ["#rrggbb" x3]}.
(function () {
  "use strict";
  var BASE = "/cosm/glb/";
  var FACE_PLANE = 0.137;
  var cache = {};

  function fetchGlb(url) {
    if (!cache[url]) {
      cache[url] = fetch(url).then(function (r) {
        if (!r.ok) throw new Error(url + " " + r.status);
        return r.arrayBuffer();
      }).then(parseGlb);
      cache[url].catch(function () { delete cache[url]; });
    }
    return cache[url];
  }

  function fetchImage(url) {
    if (!cache[url]) {
      cache[url] = fetch(url).then(function (r) {
        if (!r.ok) throw new Error(url + " " + r.status);
        return r.blob();
      }).then(function (b) { return createImageBitmap(b); });
      cache[url].catch(function () { delete cache[url]; });
    }
    return cache[url];
  }

  // ---------------------------------------------------------------------------------------------------------------
  // glb -> parts: [{pos Float32Array (skinned), nrm, uv, outline (per-vertex width), idx, mat {kind, image}}]

  var COMPONENTS = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT4: 16 };
  var TYPES = { 5120: Int8Array, 5121: Uint8Array, 5122: Int16Array, 5123: Uint16Array, 5125: Uint32Array, 5126: Float32Array };
  var NORM = { 5120: 127, 5121: 255, 5122: 32767, 5123: 65535 };

  function readAccessor(doc, bin, i, asFloat) {
    var a = doc.accessors[i], v = doc.bufferViews[a.bufferView], n = COMPONENTS[a.type], T = TYPES[a.componentType];
    var stride = v.byteStride || n * T.BYTES_PER_ELEMENT, off = (v.byteOffset || 0) + (a.byteOffset || 0);
    var out = asFloat ? new Float32Array(a.count * n) : new T(a.count * n);
    var scale = asFloat && a.normalized && NORM[a.componentType] ? 1 / NORM[a.componentType] : 1;
    for (var k = 0; k < a.count; k++) {
      var src = new T(bin.buffer, bin.byteOffset + off + k * stride, n);
      for (var c = 0; c < n; c++) out[k * n + c] = src[c] * scale;
    }
    return { data: out, n: n, count: a.count };
  }

  function parseGlb(buf) {
    var dv = new DataView(buf);
    if (dv.getUint32(0, true) !== 0x46546c67) throw new Error("not a glb");
    var jsonLen = dv.getUint32(12, true);
    var doc = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, 20, jsonLen)));
    var binOff = 20 + jsonLen;
    var bin = new Uint8Array(buf, binOff + 8, dv.getUint32(binOff, true));
    return { doc: doc, bin: bin };
  }

  function mul(a, b) { // column-major 4x4
    var o = new Float32Array(16);
    for (var c = 0; c < 4; c++)
      for (var r = 0; r < 4; r++)
        o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    return o;
  }

  function trs(n) {
    if (n.matrix) return new Float32Array(n.matrix);
    var t = n.translation || [0, 0, 0], q = n.rotation || [0, 0, 0, 1], s = n.scale || [1, 1, 1];
    var x = q[0], y = q[1], z = q[2], w = q[3];
    return new Float32Array([
      (1 - 2 * (y * y + z * z)) * s[0], (2 * (x * y + z * w)) * s[0], (2 * (x * z - y * w)) * s[0], 0,
      (2 * (x * y - z * w)) * s[1], (1 - 2 * (x * x + z * z)) * s[1], (2 * (y * z + x * w)) * s[1], 0,
      (2 * (x * z + y * w)) * s[2], (2 * (y * z - x * w)) * s[2], (1 - 2 * (x * x + y * y)) * s[2], 0,
      t[0], t[1], t[2], 1]);
  }

  function worlds(doc) {
    var out = new Array(doc.nodes.length), parent = {};
    doc.nodes.forEach(function (n, i) { (n.children || []).forEach(function (c) { parent[c] = i; }); });
    function get(i) {
      if (!out[i]) out[i] = parent[i] === undefined ? trs(doc.nodes[i]) : mul(get(parent[i]), trs(doc.nodes[i]));
      return out[i];
    }
    doc.nodes.forEach(function (_, i) { get(i); });
    return out;
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
    var doc = g.doc, bin = g.bin, W = worlds(doc), parts = [];
    var images = (doc.images || []).map(function (im) {
      var v = doc.bufferViews[im.bufferView];
      return new Blob([bin.subarray(v.byteOffset || 0, (v.byteOffset || 0) + v.byteLength)], { type: im.mimeType || "image/png" });
    });
    doc.nodes.forEach(function (node, ni) {
      if (node.mesh === undefined) return;
      doc.meshes[node.mesh].primitives.forEach(function (p) {
        if (p.mode !== undefined && p.mode !== 4) return;
        var at = p.attributes;
        var pos = readAccessor(doc, bin, at.POSITION, true).data;
        var nrm = at.NORMAL !== undefined ? readAccessor(doc, bin, at.NORMAL, true).data : new Float32Array(pos.length);
        var uv = at.TEXCOORD_0 !== undefined ? readAccessor(doc, bin, at.TEXCOORD_0, true).data : new Float32Array(pos.length / 3 * 2);
        var col = at.COLOR_0 !== undefined ? readAccessor(doc, bin, at.COLOR_0, true) : null;
        var count = pos.length / 3, m = doc.materials[p.material] || {};
        var kind = materialKind(m);
        if (kind === "face") { // chara_3d.cpp load_part: the face plane to the costume's face size
          var mini = (m.name || "").toLowerCase().indexOf("body_027000_02_") === 0;
          var mnx = 1e9, mxx = -1e9, mny = 1e9, mxy = -1e9, mnz = 1e9, mxz = -1e9;
          for (var v = 0; v < count; v++) {
            mnx = Math.min(mnx, pos[v * 3]); mxx = Math.max(mxx, pos[v * 3]);
            mny = Math.min(mny, pos[v * 3 + 1]); mxy = Math.max(mxy, pos[v * 3 + 1]);
            mnz = Math.min(mnz, pos[v * 3 + 2]); mxz = Math.max(mxz, pos[v * 3 + 2]);
          }
          var size = Math.max(mxx - mnx, mxy - mny);
          var plate = isHead || size > FACE_PLANE * 1.02 ? FACE_PLANE : size;
          var target = mini ? plate : plate * 4 / 3;
          if (size > 0.0001) {
            var f = target / size, cx = (mnx + mxx) / 2, cy = (mny + mxy) / 2, cz = (mnz + mxz) / 2;
            for (v = 0; v < count; v++) {
              pos[v * 3] = cx + (pos[v * 3] - cx) * f;
              pos[v * 3 + 1] = cy + (pos[v * 3 + 1] - cy) * f;
              pos[v * 3 + 2] = cz + (pos[v * 3 + 2] - cz) * f;
            }
          }
        }
        // the mesh as stored is the bind pose, which is what the game draws without an animation (raylib's
        // LoadModel keeps a skinned mesh's vertices as they are); only an unskinned mesh takes its node's transform
        var P = pos, N = nrm, M0 = node.skin === undefined ? W[ni] : null;
        if (M0) {
          P = new Float32Array(pos.length); N = new Float32Array(pos.length);
          for (var i = 0; i < count; i++) {
            var x = pos[i * 3], y = pos[i * 3 + 1], z = pos[i * 3 + 2], nx = nrm[i * 3], ny = nrm[i * 3 + 1], nz = nrm[i * 3 + 2];
            P[i * 3] = M0[0] * x + M0[4] * y + M0[8] * z + M0[12];
            P[i * 3 + 1] = M0[1] * x + M0[5] * y + M0[9] * z + M0[13];
            P[i * 3 + 2] = M0[2] * x + M0[6] * y + M0[10] * z + M0[14];
            var qx = M0[0] * nx + M0[4] * ny + M0[8] * nz, qy = M0[1] * nx + M0[5] * ny + M0[9] * nz,
                qz = M0[2] * nx + M0[6] * ny + M0[10] * nz, l = Math.hypot(qx, qy, qz) || 1;
            N[i * 3] = qx / l; N[i * 3 + 1] = qy / l; N[i * 3 + 2] = qz / l;
          }
        }
        var outline = new Float32Array(count);
        if (col && kind !== "face") for (var i = 0; i < count; i++) outline[i] = col.data[i * col.n + 1];
        var idx = p.indices !== undefined ? readAccessor(doc, bin, p.indices, false).data : null;
        if (!idx) { idx = new Uint32Array(count); for (var j = 0; j < count; j++) idx[j] = j; }
        var tex = m.pbrMetallicRoughness && m.pbrMetallicRoughness.baseColorTexture;
        var image = tex ? images[doc.textures[tex.index].source] : null;
        parts.push({
          pos: P, nrm: N, uv: uv, outline: outline, idx: new Uint32Array(idx), kind: kind, image: image,
          recolor: (m.extras || {}).shaderType === "taikoEffectChangeColors" && kind !== "face",
          twoSided: (m.name || "").toLowerCase().indexOf("cullnone") >= 0 || !!m.doubleSided
        });
      });
    });
    return parts;
  }

  // ---------------------------------------------------------------------------------------------------------------
  // drawing

  var VS = "#version 300 es\nin vec3 p;in vec3 n;in vec2 t;in float o;uniform mat4 mvp;uniform mat3 rot;uniform vec2 px;" +
    "uniform float outlinePass;out vec2 uv;void main(){vec4 c=mvp*vec4(p,1.0);if(outlinePass>0.5){vec3 m=rot*n;" +
    "if(dot(m.xy,m.xy)>1e-12){c.xy+=px*o*normalize(m.xy)*c.w;}}gl_Position=c;uv=t;}";
  var FS = "#version 300 es\nprecision mediump float;in vec2 uv;uniform sampler2D tex;uniform int mode;uniform vec3 c1;" +
    "uniform vec3 c2;uniform vec3 c3;uniform float faceRows;out vec4 o;void main(){gl_FragDepth=gl_FragCoord.z;" +
    "if(mode==3){o=vec4(0.0,0.0,0.0,1.0);return;}" +
    "vec2 st=uv;if(mode==2){st=vec2(uv.x,clamp(uv.y,0.0,1.0)/faceRows);}vec4 x=texture(tex,st);" +
    "if(mode==2){if(x.a<0.01)discard;gl_FragDepth=gl_FragCoord.z-0.001;o=x;return;}" +
    "if(x.a<=0.0)discard;if(mode==1){float s=max(x.r,max(x.g,x.b)),w=min(x.r,min(x.g,x.b));" +
    "if(s>0.05&&(s-w)>0.08){if(x.b>x.r&&x.b>=x.g)x.rgb=c3;else if(x.g>x.r&&x.g>x.b)x.rgb=c2;else x.rgb=c1;}}o=x;}";

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
    if (!gl) return;
    var prog = gl.createProgram();
    [[gl.VERTEX_SHADER, VS], [gl.FRAGMENT_SHADER, FS]].forEach(function (s) {
      var sh = gl.createShader(s[0]);
      gl.shaderSource(sh, s[1]);
      gl.compileShader(sh);
      if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(sh));
      gl.attachShader(prog, sh);
    });
    gl.bindAttribLocation(prog, 0, "p");
    gl.bindAttribLocation(prog, 1, "n");
    gl.bindAttribLocation(prog, 2, "t");
    gl.bindAttribLocation(prog, 3, "o");
    gl.linkProgram(prog);
    this.prog = prog;
    this.u = {};
    var self = this;
    ["mvp", "rot", "px", "outlinePass", "tex", "mode", "c1", "c2", "c3", "faceRows"].forEach(function (n) {
      self.u[n] = gl.getUniformLocation(prog, n);
    });
    // drag to turn
    var drag = null;
    this.spin = true;
    canvas.style.touchAction = "none";
    canvas.addEventListener("pointerdown", function (e) { drag = e.clientX; self.spin = false; canvas.setPointerCapture(e.pointerId); });
    canvas.addEventListener("pointermove", function (e) {
      if (drag === null) return;
      self.angle += (e.clientX - drag) * 0.012;
      drag = e.clientX;
      self.draw();
    });
    canvas.addEventListener("pointerup", function () { drag = null; });
    // until touched it sways a little either side of facing front
    function tick(t) {
      if (self.spin && self.parts.length && document.visibilityState === "visible") {
        self.angle = 0.5 * Math.sin(t * 0.0007);
        self.draw();
      }
      requestAnimationFrame(tick);
    }
    requestAnimationFrame(tick);
  }

  Viewer.prototype.upload = function (part, bitmap) {
    var gl = this.gl;
    var vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    [[part.pos, 3], [part.nrm, 3], [part.uv, 2], [part.outline, 1]].forEach(function (a, i) {
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
    if (bitmap) {
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, bitmap);
    } else {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([255, 255, 255, 255]));
    }
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, part.kind === "face" ? gl.CLAMP_TO_EDGE : gl.REPEAT);
    return { vao: vao, count: part.idx.length, tex: tex, kind: part.kind, recolor: part.recolor, twoSided: part.twoSided,
             bufs: [ib] };
  };

  Viewer.prototype.set = function (look) {
    var self = this, gl = this.gl, serial = ++this.serial;
    this.look = look;
    if (!gl) return Promise.resolve();
    var files = look.mode === "costume" ? [["cos", look.costume, false]] : [["body", look.body, false], ["head", look.head, true]];
    var key = JSON.stringify(files);
    if (key === this.key) { this.draw(); return Promise.resolve(); }
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
        self.bounds(parts);
        self.key = key;
        self.canvas.classList.remove("loading", "failed");
        self.draw();
      });
    }).catch(function () {
      if (serial !== self.serial) return;
      self.canvas.classList.remove("loading");
      self.canvas.classList.add("failed");
    });
  };

  Viewer.prototype.drop = function () {
    var gl = this.gl;
    this.parts.forEach(function (p) {
      gl.deleteVertexArray(p.vao);
      gl.deleteTexture(p.tex);
    });
    this.parts = [];
  };

  Viewer.prototype.bounds = function (parts) {
    var mn = [1e9, 1e9, 1e9], mx = [-1e9, -1e9, -1e9];
    parts.forEach(function (p) {
      if (p.kind === "face") return;
      for (var i = 0; i < p.pos.length; i += 3)
        for (var c = 0; c < 3; c++) { mn[c] = Math.min(mn[c], p.pos[i + c]); mx[c] = Math.max(mx[c], p.pos[i + c]); }
    });
    this.center = [(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2];
    this.radius = Math.max(mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]) * 0.62 || 1;
  };

  Viewer.prototype.draw = function () {
    var gl = this.gl, c = this.canvas;
    if (!gl || !this.parts.length) return;
    var dpr = window.devicePixelRatio || 1, w = Math.round(c.clientWidth * dpr), h = Math.round(c.clientHeight * dpr);
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
    gl.viewport(0, 0, w, h);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.useProgram(this.prog);
    // orthographic, centred on the model, turned about the vertical axis; the Don faces +z (the viewer at angle 0)
    var r = this.radius, asp = w / h, sx = asp >= 1 ? r * asp : r, sy = asp >= 1 ? r : r / asp;
    var ca = Math.cos(this.angle), sa = Math.sin(this.angle), cx = this.center[0], cy = this.center[1], cz = this.center[2];
    var rot = [ca, 0, -sa, 0, 1, 0, sa, 0, ca];
    var mvp = new Float32Array([
      ca / sx, 0, sa * 0.5 / r, 0,
      0, 1 / sy, 0, 0,
      sa / sx, 0, -ca * 0.5 / r, 0,
      (-ca * cx - sa * cz) / sx, -cy / sy, (ca * cz - sa * cx) * 0.5 / r, 1]);
    gl.uniformMatrix4fv(this.u.mvp, false, mvp);
    gl.uniformMatrix3fv(this.u.rot, false, new Float32Array(rot));
    gl.uniform2f(this.u.px, 2.5 * dpr * (h / 300) * 2 / w, 2.5 * dpr * (h / 300) * 2 / h);
    var colors = (this.look && this.look.colors) || [];
    gl.uniform3fv(this.u.c1, hex(colors[0]));
    gl.uniform3fv(this.u.c2, hex(colors[1]));
    gl.uniform3fv(this.u.c3, hex(colors[2]));
    gl.uniform1f(this.u.faceRows, this.faceRows || 1);
    gl.uniform1i(this.u.tex, 0);
    gl.activeTexture(gl.TEXTURE0);
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LEQUAL);
    gl.frontFace(gl.CCW);
    var self = this;
    function pass(filter, mode, outline) {
      self.parts.forEach(function (p) {
        if (!filter(p)) return;
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
    gl.disable(gl.BLEND);
    gl.depthMask(true);
    pass(function (p) { return p.kind === "opaque"; }, 0, true);   // the outline hull first
    gl.enable(gl.BLEND);
    gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    pass(function (p) { return p.kind === "opaque"; });
    pass(function (p) { return p.kind === "face"; });
    gl.depthMask(false);
    pass(function (p) { return p.kind === "blend"; });
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE);
    pass(function (p) { return p.kind === "add"; });
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
