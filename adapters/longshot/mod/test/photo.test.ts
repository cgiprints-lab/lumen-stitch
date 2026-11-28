// Lumen Photo Suite's tests, against the fake game (Longshot.exe mod test):
// game.camera.view is the view the game would draw, game.input.gameReads what
// Link reads, game.claims who holds what, game.paused / game.hudHidden the
// holds this mod takes. The fake game has no capture seam, which is also a
// tested state: the mod must plan, log, and write nothing.

import type { FakeGame, Tests } from "longshot/testing";

function view(game: FakeGame) {
  const v = game.camera.view;
  if (!v)
    throw new Error("no camera view");
  return v;
}

function logsMatching(game: FakeGame, re: RegExp): string[] {
  return game.logs.map((l) => l.text).filter((t) => re.test(t));
}

const enterPhoto = async (game: FakeGame) => {
  game.enterMap("Field");
  game.camera.set({ position: { x: 0, y: 2, z: 5 }, target: { x: 0, y: 2, z: 0 }, fov: 50 });
  // (the game's camera is watched from the mod's first frame; F8 once the view is known)
  await game.frames(1);
  game.input.holdKey("F8");
  await game.frames(1);
  game.input.releaseKey("F8");
  await game.frames(1);
};

export default {
  "F8 takes the camera, freezes the world, hides the HUD, and takes the pad": async (game, assert) => {
    await enterPhoto(game);
    assert.equal(view(game).overridden, true);
    assert.equal(game.paused, true);
    assert.equal(game.hudHidden, true);
    assert.deepEqual(game.claims, { camera: "lumen/longshot-photo", "input.pad": "lumen/longshot-photo" });
    game.input.setPad({ buttons: { a: true }, left: { x: 0, y: 1 } });
    await game.frames(1);
    assert.equal(game.input.gameReads.buttons.a, false);
    assert.deepEqual(game.input.gameReads.left, { x: 0, y: 0 });
  },

  "F8 again gives every hold back": async (game, assert) => {
    await enterPhoto(game);
    game.input.holdKey("F8");
    await game.frames(1);
    assert.equal(view(game).overridden, false);
    assert.equal(game.paused, false);
    assert.equal(game.hudHidden, false);
    assert.deepEqual(game.claims, {});
    game.input.setPad({ buttons: { a: true } });
    await game.frames(1);
    assert.equal(game.input.gameReads.buttons.a, true);
  },

  "the left stick flies at the setting's speed (8 m/s default)": async (game, assert) => {
    await enterPhoto(game);
    game.input.setPad({ left: { x: 0, y: 1 } });
    await game.frames(31, { dt: 1 / 30 });   // (the stick is read after the first frame: 30 frames of flight)
    const v = view(game);
    assert.ok(Math.abs(v.position.z - (5 - 8)) < 1e-6, "flew 8 m toward -z: " + JSON.stringify(v.position));
  },

  "D-pad right / left cycle the job": async (game, assert) => {
    await enterPhoto(game);
    game.input.setPad({ buttons: { dpadRight: true } });
    await game.frames(2);
    game.input.setPad({ buttons: {} });
    await game.frames(1);
    assert.ok(logsMatching(game, /lumen: job stereo/).length >= 1, JSON.stringify(game.logs));
    game.input.setPad({ buttons: { dpadLeft: true } });
    await game.frames(2);
    assert.ok(logsMatching(game, /lumen: job single/).length >= 1, JSON.stringify(game.logs));
  },

  "A with no capture seam: the single-frame plan is logged, nothing is written": async (game, assert) => {
    await enterPhoto(game);
    game.input.setPad({ buttons: { a: true } });
    await game.frames(2);
    assert.ok(logsMatching(game, /plan single frame/).length >= 1, JSON.stringify(game.logs));
    assert.ok(logsMatching(game, /no capture seam/).length >= 1, "the missing seam is named");
  },

  "a gigapixel plan names its 16 tiles for the stitcher": async (game, assert) => {
    await enterPhoto(game);
    game.input.setPad({ buttons: { dpadRight: true } });
    await game.frames(2);
    game.input.setPad({ buttons: {} });          // (single -> stereo)
    await game.frames(1);
    game.input.setPad({ buttons: { dpadRight: true } });
    await game.frames(2);
    game.input.setPad({ buttons: {} });          // (stereo -> gigapixel)
    await game.frames(1);
    game.input.setPad({ buttons: { a: true } });
    await game.frames(2);
    game.input.setPad({ buttons: {} });
    assert.ok(logsMatching(game, /plan gigapixel 16 tiles/).length >= 1, JSON.stringify(game.logs));
  },

  "a build without the pause keeps flying over a live world": async (game, assert) => {
    game.allowPause(false);
    await enterPhoto(game);
    assert.equal(view(game).overridden, true);
    assert.equal(game.paused, false);
    assert.deepEqual(game.claims, { camera: "lumen/longshot-photo", "input.pad": "lumen/longshot-photo" });
  },
} satisfies Tests;
