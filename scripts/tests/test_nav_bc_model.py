"""Focused export/reference checks. Run on Ryzen, never on the development Mac.

    python -m unittest discover -s scripts/tests -p test_nav_bc_model.py

The trained, 1,000-step parity check is performed by the training/evaluation
script; these small fixtures isolate the weight format, GRU state, and odd
adaptive-pooling bins.
"""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np
import torch
from torch.nn import functional as F

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from nav_bc_model import HEAD_SIZES, BC3_HEAD_SIZES, NavPolicy, export_weights
from nav_bc_numpy import NumpyPolicy, _adaptive_avg_pool2d
from nav_bc_data import BC4_PREPROCESSING, HD_PREPROCESSING, PREPROCESSING, VECTOR_SIZE


class NavPolicyReferenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        torch.set_num_threads(1)

    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "policy.weights"
        torch.manual_seed(205)
        self.model = NavPolicy(vector_size=37).eval()
        self.preprocessing = dict(PREPROCESSING)
        self.header = export_weights(self.model, self.path, self.preprocessing)
        self.policy = NumpyPolicy(self.path)

    def test_single_file_export_round_trip_and_offsets(self) -> None:
        self.assertEqual(list(Path(self.directory.name).iterdir()), [self.path])
        with self.path.open("rb") as stream:
            header = json.loads(stream.readline())
            payload = stream.read()
        self.assertEqual(header, self.header)
        self.assertEqual(header["preprocessing"], self.preprocessing)
        self.assertEqual(header["preprocessing"]["navdemo_version"], 2)
        self.assertEqual(header["preprocessing"]["roads_fields"], [
            "carrot_direction", "carrot_distance_cm", "goal_direction",
            "goal_distance_cm", "remaining_known_cm", "beyond_known"])
        self.assertNotIn("previous_keys", header["preprocessing"]["input_fields"])
        self.assertEqual(len(payload), header["data_nbytes"])
        offset = 0
        state = self.model.state_dict()
        self.assertEqual(set(self.policy.weights), set(state))
        for entry in header["tensors"]:
            self.assertEqual(entry["offset"], offset)
            self.assertEqual(entry["dtype"], "float32")
            array = self.policy.weights[entry["name"]]
            np.testing.assert_array_equal(array, state[entry["name"]].numpy())
            self.assertEqual(list(array.shape), entry["shape"])
            self.assertEqual(array.nbytes, entry["nbytes"])
            offset += entry["nbytes"]
        self.assertEqual(offset, len(payload))

    def test_sequence_matches_torch_with_hidden_carry_and_episode_reset(self) -> None:
        rng = np.random.default_rng(719)
        length, boundary = 15, 6
        near = rng.standard_normal((1, length, 9, 32, 32), dtype=np.float32)
        far = rng.standard_normal((1, length, 9, 25, 25), dtype=np.float32)
        vector = rng.standard_normal((1, length, 37), dtype=np.float32)
        inputs = tuple(torch.from_numpy(value) for value in (near, far, vector))
        with torch.inference_mode():
            whole_logits, whole_hidden = self.model(*inputs)
            first_logits, carried = self.model(*(value[:, :boundary] for value in inputs))
            rest_logits, carried = self.model(*(value[:, boundary:] for value in inputs), carried)
        for name in HEAD_SIZES:
            segmented = torch.cat((first_logits[name], rest_logits[name]), dim=1)
            torch.testing.assert_close(segmented, whole_logits[name], rtol=1e-5, atol=1e-6)
        torch.testing.assert_close(carried, whole_hidden, rtol=1e-5, atol=1e-6)

        hidden = None
        for step in range(length):
            logits, hidden = self.policy.step(near[0, step], far[0, step], vector[0, step], hidden)
            for name in HEAD_SIZES:
                np.testing.assert_allclose(logits[name], whole_logits[name][0, step].numpy(), rtol=0, atol=1e-5)
        np.testing.assert_allclose(hidden, whole_hidden[0, 0].numpy(), rtol=0, atol=1e-5)
        self.assertGreater(float(np.linalg.norm(hidden)), 0.01)

        # A new episode must reset state. Omitted state is equivalent to
        # explicit zeros, and both match torch's fresh recurrent sequence.
        fresh, fresh_hidden = self.policy.step(near[0, 0], far[0, 0], vector[0, 0])
        zeroed, zeroed_hidden = self.policy.step(
            near[0, 0], far[0, 0], vector[0, 0], np.zeros(256, dtype=np.float32)
        )
        for name in HEAD_SIZES:
            np.testing.assert_array_equal(fresh[name], zeroed[name])
            np.testing.assert_allclose(fresh[name], whole_logits[name][0, 0].numpy(), rtol=0, atol=1e-5)
        np.testing.assert_array_equal(fresh_hidden, zeroed_hidden)

    def test_odd_adaptive_pooling_matches_torch_overlapping_bins(self) -> None:
        values = np.arange(16 * 7 * 7, dtype=np.float32).reshape(16, 7, 7)
        # These impulses belong to both bins; non-overlapping pooling loses one.
        values[:, 3, 3] += np.float32(1000)
        expected = F.adaptive_avg_pool2d(torch.from_numpy(values), (2, 2)).numpy().reshape(-1)
        np.testing.assert_array_equal(_adaptive_avg_pool2d(values), expected)

    def test_truncated_payload_is_rejected(self) -> None:
        self.path.write_bytes(self.path.read_bytes()[:-3])
        with self.assertRaisesRegex(ValueError, "payload length"):
            NumpyPolicy(self.path)

    def test_tensor_range_outside_payload_is_rejected(self) -> None:
        with self.path.open("rb") as stream:
            header = json.loads(stream.readline())
            payload = stream.read()
        header["tensors"][0]["offset"] = len(payload)
        self.path.write_bytes(json.dumps(header).encode("utf-8") + b"\n" + payload)
        with self.assertRaisesRegex(ValueError, "invalid payload range"):
            NumpyPolicy(self.path)

    def test_bc3_auxiliary_heads_are_training_only_and_recurrent_logits_match(self) -> None:
        for version in (2, 3, 4):
            with self.subTest(version=version):
                model = NavPolicy(37, hidden_size=16, format_version=version, auxiliary=True).eval()
                preprocessing = BC4_PREPROCESSING if version == 4 else HD_PREPROCESSING if version == 3 else self.preprocessing
                temperatures = dict.fromkeys(BC3_HEAD_SIZES, .8)
                header = export_weights(model, self.path, preprocessing, temperatures)
                self.assertEqual(header["version"], version)
                self.assertEqual(header["temperatures"], temperatures)
                self.assertEqual(header["architecture"]["heads"]["maneuver"]["values"], [0, 1, 2])
                self.assertFalse(any(tensor["name"].startswith("aux_") for tensor in header["tensors"]))
                policy = NumpyPolicy(self.path)
                self.assertEqual(policy.near_shape, model.near_shape)
                self.assertEqual(policy.far_shape, model.far_shape)
                rng = np.random.default_rng(14)
                near = rng.normal(size=(1, 5, *model.near_shape)).astype(np.float32)
                far = rng.normal(size=(1, 5, *model.far_shape)).astype(np.float32)
                vector = rng.normal(size=(1, 5, 37)).astype(np.float32)
                with torch.inference_mode():
                    logits, hidden, auxiliary = model(*(torch.from_numpy(a) for a in (near, far, vector)),
                                                       return_auxiliary=True)
                self.assertEqual(auxiliary["next_near"].shape, (1, 5, 9, 32, 32))
                carried = None
                for step in range(5):
                    actual, carried = policy.step(near[0, step], far[0, step], vector[0, step], carried)
                    for head in BC3_HEAD_SIZES:
                        np.testing.assert_allclose(actual[head], logits[head][0, step].numpy(), atol=1e-5)
                np.testing.assert_allclose(carried, hidden[0, 0].numpy(), atol=1e-5)

    def test_hd_architecture_and_macs_keep_compact_auxiliary_target(self) -> None:
        compact = NavPolicy(VECTOR_SIZE, format_version=2, auxiliary=True)
        hd = NavPolicy(VECTOR_SIZE, format_version=3, auxiliary=True)
        self.assertEqual(hd.near_shape, (10, 64, 64))
        self.assertEqual(hd.far_shape, (10, 50, 100))
        architecture = hd.architecture()
        self.assertEqual(architecture["version"], 1)
        self.assertEqual(architecture["inputs"]["near"], [10, 64, 64])
        self.assertEqual(architecture["inputs"]["far"], [10, 50, 100])
        self.assertEqual(architecture["map_encoders"]["layers"][0]["in_channels"], 10)
        self.assertEqual(compact.mac_count()["total"], 1_014_088)
        self.assertEqual(hd.mac_count()["total"], 2_915_104)
        self.assertEqual(hd.aux_next_near.weight.shape, compact.aux_next_near.weight.shape)
        with self.assertRaisesRegex(ValueError, "MAC decision budget"):
            NavPolicy(20_000, format_version=2)

    def test_hd_initialization_resets_entire_map_encoders_and_transfers_other_tensors(self) -> None:
        from nav_bc_train import initialize_bc3_hd
        from nav_demos import RAY_COUNT

        config = dict(vector_size=VECTOR_SIZE, hidden_size=16, format_version=2, auxiliary=True)
        source = NavPolicy(**config)
        with torch.no_grad():
            for index, parameter in enumerate(source.parameters()):
                parameter.fill_((index + 1) / 100)
        source_state = {name: value.clone() for name, value in source.state_dict().items()}
        checkpoint = Path(self.directory.name) / "last.pt"
        torch.save(dict(config=config, model=source_state), checkpoint)
        eye_layout = np.tile([0., 0., 10_000.], (RAY_COUNT, 1)).tolist()
        export_weights(source, checkpoint.parent / "policy.navbc",
                       {**PREPROCESSING, "eye_layout": eye_layout})

        hd = NavPolicy(VECTOR_SIZE, hidden_size=16, format_version=3, auxiliary=True)
        initial = {name: value.clone() for name, value in hd.state_dict().items()}
        result = initialize_bc3_hd(hd, checkpoint, {"eye_layout": eye_layout})

        reset, transferred = set(), set()
        for name, actual in hd.state_dict().items():
            with self.subTest(tensor=name):
                if name.startswith(("near_encoder.", "far_encoder.")):
                    reset.add(name)
                    self.assertTrue(torch.equal(actual, initial[name]))
                    if source_state[name].shape == initial[name].shape:
                        self.assertFalse(torch.equal(initial[name], source_state[name]))
                else:
                    transferred.add(name)
                    self.assertTrue(torch.equal(actual, source_state[name]))
        self.assertEqual(set(result["reset"]), reset)
        self.assertEqual(set(result["transferred"]), transferred)
        self.assertTrue(any(name.startswith("aux_") for name in transferred))

        # BC4 must instead preserve all HD tensors, adding only a zero familiar
        # input slice so its initial logits remain the HD policy's logits.
        hd_state = {name: value.clone() for name, value in hd.state_dict().items()}
        torch.save(dict(config=dict(config, format_version=3), model=hd_state), checkpoint)
        export_weights(hd, checkpoint.parent / "policy.navbc",
                       {**HD_PREPROCESSING, "eye_layout": eye_layout})
        bc4 = NavPolicy(VECTOR_SIZE, hidden_size=16, format_version=4, auxiliary=True)
        result = initialize_bc3_hd(bc4, checkpoint, {"eye_layout": eye_layout})
        self.assertEqual(result["reset"], [])
        self.assertEqual(set(result["expanded"]), {"near_encoder.0.weight", "far_encoder.0.weight"})
        for name, actual in bc4.state_dict().items():
            if name in result["expanded"]:
                self.assertTrue(torch.equal(actual[:, :10], hd_state[name]))
                self.assertFalse(actual[:, 10].any())
            else:
                self.assertTrue(torch.equal(actual, hd_state[name]))

    def test_hd_sampling_decodes_one_mutually_exclusive_maneuver(self) -> None:
        model = NavPolicy(37, hidden_size=16, format_version=3)
        export_weights(model, self.path, HD_PREPROCESSING)
        policy = NumpyPolicy(self.path)
        for action in range(3):
            logits = {name: np.zeros(classes, dtype=np.float32) for name, classes in BC3_HEAD_SIZES.items()}
            logits["maneuver"][:] = -1000
            logits["maneuver"][action] = 1000
            keys = policy.sample(logits, np.random.default_rng(3))
            self.assertEqual(keys["jump"], action == 1)
            self.assertEqual(keys["climb"], action == 2)

    def test_hd_requires_matching_preprocessing_and_map_shapes(self) -> None:
        model = NavPolicy(37, hidden_size=16, format_version=3)
        export_weights(model, self.path, self.preprocessing)
        with self.assertRaisesRegex(ValueError, "hd preprocessing"):
            NumpyPolicy(self.path)
        export_weights(model, self.path, HD_PREPROCESSING)
        policy = NumpyPolicy(self.path)
        with self.assertRaisesRegex(ValueError, "expected near"):
            policy.step(np.zeros((9, 32, 32)), np.zeros((9, 25, 25)), np.zeros(37))
        with self.path.open("rb") as stream:
            header, payload = json.loads(stream.readline()), stream.read()
        header["architecture"]["inputs"]["far"] = [10, 100, 100]
        self.path.write_bytes(json.dumps(header).encode() + b"\n" + payload)
        with self.assertRaisesRegex(ValueError, "unsupported policy map shapes"):
            NumpyPolicy(self.path)

    def test_export_rejects_invalid_head_temperatures(self) -> None:
        for temperatures in ({"turn": 1.}, dict.fromkeys(HEAD_SIZES, 0.),
                             dict.fromkeys(HEAD_SIZES, float("nan"))):
            with self.assertRaisesRegex(ValueError, "temperatures"):
                export_weights(self.model, self.path, temperatures=temperatures)


if __name__ == "__main__":
    unittest.main()
