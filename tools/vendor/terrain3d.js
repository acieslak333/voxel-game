// Live 3D terrain view shared by the worldgen + biome web tools. Meshes the engine's
// heightfield export (voxelgame --genmap --mode height -> /heightmap.png) into a
// displaced, vertex-coloured, lit terrain with a flat sea plane + gradient sky. The
// host page provides #main (view area), #gl (canvas) and #ex (height slider), owns the
// on/off state, and calls t3dShow(on) + t3dBuild(url). Uses globals THREE + OrbitControls
// (vendored locally, so it works offline).
(function () {
  let renderer, scene, camera, controls, mesh, water;
  const SPAN = 180; // world units the terrain slice spans in X/Z

  window.t3dInit = function () {
    if (renderer) return;
    const canvas = document.getElementById('gl');
    renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
    scene = new THREE.Scene();
    scene.background = new THREE.Color(0x2a3a52);
    // Gradient sky dome (bright near the horizon, deep blue overhead).
    const sky = new THREE.Mesh(new THREE.SphereGeometry(3000, 24, 12),
      new THREE.ShaderMaterial({
        side: THREE.BackSide,
        vertexShader: 'varying vec3 wp; void main(){ wp=position; gl_Position=projectionMatrix*modelViewMatrix*vec4(position,1.0); }',
        fragmentShader: 'varying vec3 wp; void main(){ float t=clamp(normalize(wp).y*0.5+0.5,0.0,1.0); gl_FragColor=vec4(mix(vec3(0.72,0.80,0.90),vec3(0.16,0.28,0.50),t),1.0); }'
      }));
    scene.add(sky);
    camera = new THREE.PerspectiveCamera(45, 1, 0.1, 8000);
    camera.position.set(0, 140, 205);
    controls = new THREE.OrbitControls(camera, canvas);
    controls.target.set(0, 12, 0);
    scene.add(new THREE.AmbientLight(0xffffff, 0.55));
    const dl = new THREE.DirectionalLight(0xffffff, 0.95);
    dl.position.set(-150, 200, 100);
    scene.add(dl);
    // Translucent sea plane; its height is set per build from /sealevel.
    water = new THREE.Mesh(new THREE.PlaneGeometry(SPAN, SPAN),
      new THREE.MeshStandardMaterial({ color: 0x2f6fb6, transparent: true, opacity: 0.55, roughness: 0.25 }));
    water.rotation.x = -Math.PI / 2;
    water.visible = false;
    scene.add(water);
    window.addEventListener('resize', t3dResize);
    (function loop() { requestAnimationFrame(loop); controls.update(); renderer.render(scene, camera); })();
    t3dResize();
  };

  window.t3dResize = function () {
    if (!renderer) return;
    const m = document.getElementById('main');
    renderer.setSize(m.clientWidth, m.clientHeight, false);
    camera.aspect = m.clientWidth / Math.max(1, m.clientHeight);
    camera.updateProjectionMatrix();
  };

  // Toggle the 3D canvas vs the 2D <img>.
  window.t3dShow = function (on) {
    const map = document.getElementById('map'), gl = document.getElementById('gl');
    if (map) map.style.display = on ? 'none' : 'block';
    if (gl) gl.style.display = on ? 'block' : 'none';
    if (on) { t3dInit(); t3dResize(); }
  };

  let framed = false; // frame the camera on the first build, then leave the user's orbit
  // Rebuild from the exposed-voxel binary (int32 N,H,count; then count*{u8 x,y,z,r,g,b}),
  // instancing one lit cube per voxel — so overhangs / caves / floating islands show.
  window.t3dBuild = function (url) {
    t3dInit();
    fetch(url).then(r => r.arrayBuffer()).then(buf => {
      const dv = new DataView(buf);
      const N = dv.getInt32(0, true), H = dv.getInt32(4, true), count = dv.getInt32(8, true);
      const bytes = new Uint8Array(buf, 12);
      if (mesh) { scene.remove(mesh); mesh.geometry.dispose(); mesh.material.dispose(); }
      const box = new THREE.BoxGeometry(1, 1, 1);
      mesh = new THREE.InstancedMesh(box, new THREE.MeshStandardMaterial({ roughness: 1, metalness: 0 }), count);
      const m = new THREE.Matrix4(), c = new THREE.Color(), off = N / 2;
      for (let i = 0; i < count; i++) {
        const p = i * 6;
        m.makeTranslation(bytes[p] - off + 0.5, bytes[p + 1] + 0.5, bytes[p + 2] - off + 0.5);
        mesh.setMatrixAt(i, m);
        c.setRGB(bytes[p + 3] / 255, bytes[p + 4] / 255, bytes[p + 5] / 255);
        mesh.setColorAt(i, c);
      }
      mesh.instanceMatrix.needsUpdate = true;
      if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true;
      scene.add(mesh);
      fetch('/sealevel').then(r => r.json()).then(d => {
        water.position.set(0, d.seaY, 0);          // sea level in block units
        const s = Math.max(N * 2.2, 200) / SPAN; water.scale.set(s, s, 1);
        water.visible = true;
        if (!framed) {
          framed = true;
          controls.target.set(0, Math.max(d.seaY, H * 0.3), 0);
          camera.position.set(N * 0.9, H * 1.05, N * 1.8);
          controls.update();
        }
      }).catch(function () {});
    }).catch(function () {});
  };
})();
