// The parts of three.js the simulator page's hands use, as one classic script
// that sets THREE on the global object (the page also opens from disk, where
// modules do not load, and the checks load it under Node). No GLTFLoader: the
// hand model comes ready in handmodel.js. Bundled by build-three.sh into
// sim/web/three.js.
import {
  Scene, Group, Object3D, Bone, Skeleton, SkinnedMesh, Mesh,
  BufferGeometry, BufferAttribute, Float32BufferAttribute, Uint16BufferAttribute,
  Matrix4, Quaternion, Vector3, Vector2, Euler, Color, Box3, MathUtils,
  WebGLRenderer, PerspectiveCamera, HemisphereLight, DirectionalLight,
  MeshPhysicalMaterial, MeshStandardMaterial, ShadowMaterial, MeshBasicMaterial,
  PlaneGeometry, SphereGeometry, Texture, PMREMGenerator,
  PCFShadowMap, SRGBColorSpace, FrontSide, DoubleSide,
} from 'three';
import { RoomEnvironment } from 'three/examples/jsm/environments/RoomEnvironment.js';

globalThis.THREE = {
  Scene, Group, Object3D, Bone, Skeleton, SkinnedMesh, Mesh,
  BufferGeometry, BufferAttribute, Float32BufferAttribute, Uint16BufferAttribute,
  Matrix4, Quaternion, Vector3, Vector2, Euler, Color, Box3, MathUtils,
  WebGLRenderer, PerspectiveCamera, HemisphereLight, DirectionalLight,
  MeshPhysicalMaterial, MeshStandardMaterial, ShadowMaterial, MeshBasicMaterial,
  PlaneGeometry, SphereGeometry, Texture, PMREMGenerator,
  PCFShadowMap, SRGBColorSpace, FrontSide, DoubleSide,
  RoomEnvironment,
};
