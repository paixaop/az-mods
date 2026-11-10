# madmom beat/downbeat models

These resources derive from the publicly released 2016 bidirectional LSTM
downbeat models by the madmom authors at the Department of Computational
Perception, Johannes Kepler University Linz. The upstream project is
[CPJKU/madmom_models](https://github.com/CPJKU/madmom_models), revision
`7e3dc1b0cad499792767074d03c38b194b9b0a79`.

Models 7 and 8 retain every original Float32 weight. Conversion concatenates
the input, forget, cell and output gates and transposes matrices for Apple's
MPSGraph and Accelerate APIs. The three filterbanks are regenerated from the
madmom logarithmic-filterbank definition. The binary property lists are a
storage conversion; the learned model is unchanged. `provenance.json` records
the upstream URLs and source/output SHA-256 hashes.

The model resources and their converted forms are licensed under
[Creative Commons Attribution-NonCommercial-ShareAlike 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/).
This permits the project's local noncommercial use. Distribution must preserve
attribution and the same license for adaptations. Commercial use requires
separate permission from the rights holder; the upstream license directs such
requests to Gerhard Widmer. `LICENSE.txt` is the upstream notice.

The madmom source formulas are BSD-2-Clause; the complete upstream notice is
retained as `MADMom-SOURCE-LICENSE.txt`. Source revision used for comparison:
`27f032e8947204902c675e5e341a3faf5dc86dae`.

From the package directory, rebuild with Python and NumPy:

```sh
python3 Scripts/convert_madmom_models.py
```

The converter downloads only the two pinned public model files, verifies their
hashes before decoding, and writes the property lists. It does not read an
installed reference application. The implementation uses Apple native
MPSGraph/Metal, with Accelerate on the simulator. It has no TensorFlow runtime
dependency.

Verification separately established numerical identity between all 38 tensors
in each public model and the installed behavior reference's corresponding
SavedModel, and exact equality of the three filterbank matrices. Generated
audio fixtures compare the independent Swift features and activations against
the original TensorFlow execution. Those reference programs and extracted
artifacts remain in ignored private evidence storage.

Apple and TensorFlow Float32 kernels are not bit-identical. The validation uses
both a maximum and a mean probability-error bound and separately verifies the
HMM's decoded events and state path against original executions. A recurrent
model7 fixture amplifies sub-micro differences in its inputs; both Apple
backends still give the original decoded beat sequence for that fixture.
