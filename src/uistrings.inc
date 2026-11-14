// Every piece of text FMVibe shows: in the editor, in the host (parameter names, remote control pages) and in the
// browser demo. One line each, STR(Name, "wording"): the code asks for a wording by its name, so the wording can
// change freely while the name stays. {1}, {2}, ... stand for what the code fills in (a node, a number, a value
// with its unit); they can move within the wording. Numbers with their units (Hz, dB, ms, st, ct, bpm), note
// names, tempo divisions and node letters (A, F1, S2) are written by the code.
//
// Some wordings are also read back when a value is typed into a knob or the host: the names in each list, Off,
// On, Open, Kill and None. Names within one list need to stay different.

// --- Names of choices, as values show them in the editor and the host

STR(LfoSine, "Sine")
STR(LfoTriangle, "Triangle")
STR(LfoSaw, "Saw")
STR(LfoSquare, "Square")
STR(LfoRandom, "Random")
STR(LfoCustom, "Custom")

STR(WaveSine, "Sine")
STR(WaveTriangle, "Triangle")
STR(WaveSaw, "Saw")
STR(WaveSquare, "Square")
STR(WavePulse, "Pulse")
STR(WaveHalfSine, "Half Sine")
STR(WaveAbsSine, "Abs Sine")
STR(WaveNoise, "Noise")
STR(WaveCustom, "Custom")
STR(WaveAltSine, "Alt Sine")
STR(WaveOrgan, "Organ")
STR(WaveVowel, "Vowel")

// How an operator reads its wave's cycle (its Warp), in its buttons' tooltips and the host.
STR(WarpOff, "Off")
STR(WarpBend, "Bend")
STR(WarpSqueeze, "Squeeze")
STR(WarpPulse, "Pulse")
STR(WarpSteps, "Steps")
STR(WarpSync, "Sync") // the wave played several times through each cycle, cut off at its end

// How an operator's unison copies spread in pitch (its Unison Curve), and where they sit (its Unison Pan), in the host.
STR(UniCurveLinear, "Linear")
STR(UniCurveCenter, "Center")
STR(UniCurveSupersaw, "Supersaw")
STR(UniCurveRandom, "Random")
STR(UniPanRamp, "Ramp")
STR(UniPanAlternate, "Alternate")
STR(UniPanRandom, "Random")

STR(FilterLowPass, "Low-pass")
STR(FilterHighPass, "High-pass")
STR(FilterBandPass, "Band-pass")
STR(FilterNotch, "Notch")
STR(FilterComb, "Comb")
// On the filter type buttons, and with a slope ("LP 24") on the graph's nodes.
STR(FilterShortLowPass, "LP")
STR(FilterShortHighPass, "HP")
STR(FilterShortBandPass, "BP")
STR(FilterShortNotch, "Notch")
STR(FilterShortComb, "Comb")

// The shapers' curves.
STR(ClipHard, "Hard")
STR(ClipSoft, "Soft")
STR(ClipAsymmetric, "Asymmetric")
STR(ClipCubic, "Cubic")
STR(ClipGentle, "Gentle")
STR(ClipSineFold, "Sine Fold")
STR(ClipFold, "Fold")
STR(ClipCustom, "Custom")

// The parametric EQ's band shapes.
STR(PeqPeak, "Peak")
STR(PeqLowShelf, "Low Shelf")
STR(PeqHighShelf, "High Shelf")
STR(PeqLowCut, "Low Cut")
STR(PeqHighCut, "High Cut")
STR(PeqNotch, "Notch")

STR(ReverbHall, "Hall")
STR(ReverbRoom, "Room")
STR(ReverbPlate, "Plate")
STR(ReverbShimmer, "Shimmer")

// The amp's models; its cabinets; the mics in front of them, with the short
// names its switch shows.
STR(AmpClean, "Clean")
STR(AmpBlues, "Blues")
STR(AmpCrunch, "Crunch")
STR(AmpLead, "Lead")
STR(AmpHeavy, "Heavy")
STR(AmpBass, "Bass")
STR(AmpFuzz, "Fuzz")
STR(CabOpen1x12, "1x12 Open")
STR(CabOpen2x12, "2x12 Open")
STR(CabClosed4x12, "4x12 Closed")
STR(CabBass4x10, "4x10 Bass")
STR(CabBass1x15, "1x15 Bass")
STR(MicDynamic, "Dynamic")
STR(MicCondenser, "Condenser")
STR(MicRibbon, "Ribbon")
STR(MicDynamicShort, "DYN")
STR(MicCondenserShort, "CND")
STR(MicRibbonShort, "RIB")

// How a route acts on the operator it goes into, in its lists, menus and the host.
STR(RoutePhase, "Phase")
STR(RouteLinearFm, "Linear FM")
STR(RouteExpFm, "Exp FM")
STR(RouteRing, "Ring")

// The effects, in the list that adds them and on the host's pages.
STR(FxEmpty, "Empty")
STR(FxShaper, "Shaper")
STR(FxDelay, "Delay")
STR(FxDjEq, "DJ EQ")
STR(FxParamEq, "Param EQ")
STR(FxFilter, "Filter")
STR(FxCompressor, "Compressor")
STR(FxMultiComp, "3-Band Comp") // retired: hosts may still show it as an effect slot's type
STR(FxReverb, "Reverb")
STR(FxPhaser, "Phaser")
STR(FxFlanger, "Flanger")
STR(FxFreqShifter, "Freq Shifter")
STR(FxDisperser, "Phase Disp")
STR(FxWidener, "Widener")
STR(FxMultiDyn, "3-Band Dynamics")
STR(FxAmp, "Amp")
STR(FxUtility, "Utility")
STR(FxChorus, "Chorus")

// What a modulation place holds.
STR(SourceEmpty, "Empty")
STR(SourceLfo, "LFO")
STR(SourceEnvelope, "Envelope")
STR(SourceRandom, "Random") // a value taken at random as each note starts
STR(SourceMidi, "MIDI") // a MIDI controller or aftertouch
STR(SourceCurve, "Curve") // an LFO of a shape drawn from points
STR(MidiInputCc, "CC") // what a MIDI source follows: a controller
STR(MidiInputPressure, "Pressure") // or aftertouch
STR(MidiInputSlide, "Slide") // or a note's slide (MPE's CC 74, CLAP's brightness expression)

// The noise wave's slope at its three named settings.
STR(NoiseWhite, "White")
STR(NoisePink, "Pink")
STR(NoiseBrown, "Brown")

// The three-band compressor's bands.
STR(BandLow, "Low")
STR(BandMid, "Mid")
STR(BandHigh, "High")

STR(ValueOff, "Off")
STR(ValueOn, "On")
STR(ValueOpen, "Open") // the delay's tone in the middle, filtering nothing
STR(ValueNone, "None") // a modulation slot's target, when it has none
STR(ValueKill, "Kill") // a DJ EQ band turned all the way down

// --- Parameter names, as hosts show them, and in the editor's undo, menus and messages

// The groups hosts sort the parameters into.
STR(ModuleGlobal, "Global")
STR(ModuleOperators, "Operators")
STR(ModuleFilters, "Filters")
STR(ModuleShapers, "Shapers")
STR(ModuleMacros, "Macros")
STR(ModuleFx, "FX")
STR(ModuleModulation, "Modulation")
STR(ModuleMatrix, "Matrix")

STR(ParamVolume, "Volume")
STR(ParamFmAmount, "FM Amount")
STR(ParamBendRange, "Bend Range")
STR(ParamPitch, "Pitch")
STR(ParamPan, "Pan")
STR(ParamVoices, "Voices")
STR(ParamMono, "Mono")
STR(ParamLegato, "Legato")
STR(ParamGlide, "Glide")
STR(ParamGlideAlways, "Glide Always")
STR(ParamFxOrder, "FX Order")
STR(ParamFxPosition, "FX Order {1}")
STR(ParamSplit, "Split {1}") // a split in the effects graph, by number; its field's name follows
STR(ParamSum, "Join {1}") // a join (a sum) in the effects graph, by number; its field's name follows
STR(ParamSplitUsed, "Used")
STR(ParamSplitBands, "Bands")
STR(ParamSplitLow, "Crossover")
STR(ParamSplitHigh, "High Crossover")
STR(ParamSoloHigh, "Solo High") // a join's solo of its split's high band, after the join's name
STR(ParamSoloLow, "Solo Low")
STR(ParamSoloMid, "Solo Mid") // a join's solo of a three-band split's middle band
STR(ParamFxOut, "FX Out") // the effects graph's output, as the node its wires go into
STR(ParamFxInput, "Input {1}") // a wire into a node of the effects graph, by number; its field's name follows
STR(ParamFxInputFrom, "From")
STR(ParamFxInputLevel, "Level")
STR(ValueFxIn, "In") // a wire coming from the voices, into the effects graph
STR(ParamModWheel, "Mod Wheel")
STR(ParamPitchBend, "Pitch Bend")
STR(ParamNote, "Note")
STR(ParamVelocitySource, "Velocity") // the velocity source, as hosts name its fields
STR(ParamBendSource, "Pitch Bend") // the pitch bend source, as hosts name its fields

// What a parameter's name starts with; the field's name follows.
STR(ParamOp, "Op {1}")
STR(ParamFilter, "Filter {1}")
STR(ParamShaper, "Shaper {1}")
STR(ParamMacro, "Macro {1}")
STR(ParamFx, "FX {1}")
STR(ParamMod, "Mod {1}")

// Operators' fields, and fields that others share.
STR(ParamOn, "On")
STR(ParamType, "Type")
STR(ParamWave, "Wave")
STR(ParamCoarse, "Coarse")
STR(ParamFine, "Fine")
STR(ParamDetune, "Detune")
STR(ParamOut, "Out")
STR(ParamAttack, "Attack")
STR(ParamHold, "Hold")
STR(ParamDecay, "Decay")
STR(ParamSustain, "Sustain")
STR(ParamRelease, "Release")
STR(ParamAttackCurve, "Attack Curve")
STR(ParamDecayCurve, "Decay Curve")
STR(ParamReleaseCurve, "Release Curve")
STR(ParamRetired, "Unused") // a param kept only for its id, hidden from hosts
STR(ParamUnison, "Unison")
STR(ParamUnisonDetune, "Unison Detune")
STR(ParamUnisonSpread, "Unison Spread")
STR(ParamPhaseRandom, "Phase Random")
STR(ParamUnisonCurve, "Unison Curve")
STR(ParamUnisonPan, "Unison Pan")
STR(ParamUnisonBlend, "Unison Blend")
STR(ParamNoiseSlope, "Noise Slope")
STR(ParamFixed, "Fixed") // the operator plays a fixed frequency instead of following the note
STR(ParamFixedFreq, "Fixed Freq")
STR(ParamFixedMult, "Fixed Mult")
STR(ParamNoteSync, "Note Sync") // hard sync: the operator's wave restarts with each cycle of the note
STR(ParamWarp, "Warp")
STR(ParamWarpAmount, "Warp Amount")
STR(ParamPartials, "Partials") // how many the Custom wave plays
STR(ParamPartial, "Partial {1}")
STR(ParamLevel, "Level")
STR(ParamPhase, "Phase")
STR(ParamSlew, "Slew")
STR(ParamCc, "CC")
STR(ParamInput, "Input")
STR(ParamRouteMode, "{1} Mode") // a route into an operator ("B → A"), how it acts on it
STR(ParamOpFeedback, "{1} Feedback") // a node (an operator, a filter, a shaper) into itself, in the matrix

// Filters' and shapers' fields.
STR(ParamCutoff, "Cutoff")
STR(ParamResonance, "Resonance")
STR(ParamKeyTrack, "Key Track")
STR(ParamDrive, "Drive")
STR(ParamSlope, "Slope")
STR(ParamCurve, "Curve")
STR(ParamOutput, "Output")
STR(ParamMix, "Mix")
STR(ParamLink, "Link")
STR(ParamSymmetric, "Symmetric")
STR(ParamToOut, "To Out")
STR(ParamPoints, "Points") // how many points a drawn shape or curve has

// Sources' fields: the macros', the note source's, the mod wheel's, and the LFOs' and envelopes'.
STR(ParamTarget, "Target {1}")
STR(ParamAmount, "Amount {1}")
STR(ParamBypass, "Bypass {1}")
STR(ParamBipolar, "Bipolar")
STR(ParamAux, "Aux {1}") // the source that scales a slot
STR(ParamAuxAmount, "Amount {1}") // how much of its depth a slot keeps, 0 to 100 %
STR(ParamSlotCurve, "Curve {1}") // how a slot bends what its source puts out, -100 to 100 %
STR(ParamLow, "Low")
STR(ParamHigh, "High")
STR(ParamShape, "Shape")
STR(ParamRate, "Rate")
STR(ParamSync, "Sync")
STR(ParamDivision, "Division")
STR(ParamFree, "Free")
STR(ParamUnipolar, "Unipolar")
STR(ParamOneShot, "One-Shot")
STR(ParamAnchor, "Anchor") // an LFO's: tied to the host's transport
STR(ParamSmooth, "Smooth")
STR(ParamChaos, "Chaos")

// Effect slots' fields, after "FX 1".
STR(ParamShaperCurve, "Shaper Curve")
STR(ParamShaperDrive, "Shaper Drive")
STR(ParamShaperOutput, "Shaper Output")
STR(ParamShaperMix, "Shaper Mix")
STR(ParamShaperLink, "Shaper Link")
STR(ParamShaperSymmetric, "Shaper Symmetric")
STR(ParamDelayTimeL, "Delay Time L")
STR(ParamDelayTimeR, "Delay Time R")
STR(ParamDelaySync, "Delay Sync")
STR(ParamDelayLink, "Delay Link")
STR(ParamDelayDivisionL, "Delay Division L")
STR(ParamDelayDivisionR, "Delay Division R")
STR(ParamDelayFeedback, "Delay Feedback")
STR(ParamDelayTone, "Delay Tone")
STR(ParamDelayWidth, "Delay Width")
STR(ParamDelayPingPong, "Delay Ping-Pong")
STR(ParamDelayMix, "Delay Mix")
STR(ParamEqLow, "DJ EQ Low")
STR(ParamEqMid, "DJ EQ Mid")
STR(ParamEqHigh, "DJ EQ High")
STR(ParamPeqBand, "EQ Band {1}")
STR(ParamPeqAmount, "EQ Amount")
STR(ParamFreq, "Freq")
STR(ParamGain, "Gain")
STR(ParamQ, "Q")
STR(ParamFilterType, "Filter Type")
STR(ParamFilterSlope, "Filter Slope")
STR(ParamFilterCutoff, "Filter Cutoff")
STR(ParamFilterResonance, "Filter Resonance")
STR(ParamFilterDrive, "Filter Drive")
STR(ParamFilterMix, "Filter Mix")
STR(ParamFilterKeyTrack, "Filter Key Track")
STR(ParamCompThreshold, "Comp Threshold")
STR(ParamCompRatio, "Comp Ratio")
STR(ParamCompKnee, "Comp Knee")
STR(ParamCompAttack, "Comp Attack")
STR(ParamCompRelease, "Comp Release")
STR(ParamCompMakeup, "Comp Makeup")
STR(ParamCompMix, "Comp Mix")
STR(ParamMcLowCrossover, "3-Band Low Crossover")
STR(ParamMcHighCrossover, "3-Band High Crossover")
STR(ParamMcBand, "3-Band {1}")
STR(ParamThreshold, "Threshold")
STR(ParamRatio, "Ratio")
STR(ParamMcAttack, "3-Band Attack")
STR(ParamMcRelease, "3-Band Release")
STR(ParamMcMix, "3-Band Mix")
STR(ParamMcOutput, "3-Band Output")
STR(ParamMcLink, "3-Band Link")
STR(ParamMdLowCrossover, "3-Band Dyn Low Crossover")
STR(ParamMdHighCrossover, "3-Band Dyn High Crossover")
STR(ParamMdBand, "3-Band Dyn {1}") // the band's name
STR(ParamAboveThreshold, "Above Threshold")
STR(ParamDownRatio, "Down Ratio")
STR(ParamBelowThreshold, "Below Threshold")
STR(ParamUpRatio, "Up Ratio")
STR(ParamMdAttack, "3-Band Dyn Attack")
STR(ParamMdRelease, "3-Band Dyn Release")
STR(ParamMdKnee, "3-Band Dyn Knee")
STR(ParamMdRange, "3-Band Dyn Range")
STR(ParamMdMix, "3-Band Dyn Mix")
STR(ParamMdOutput, "3-Band Dyn Output")
STR(ParamMdLink, "3-Band Dyn Link")
STR(ParamMdRms, "3-Band Dyn RMS")
STR(ParamReverbType, "Reverb Type")
STR(ParamReverbSize, "Reverb Size")
STR(ParamReverbDecay, "Reverb Decay")
STR(ParamReverbDamping, "Reverb Damping")
STR(ParamReverbPreDelay, "Reverb Pre-Delay")
STR(ParamReverbWidth, "Reverb Width")
STR(ParamReverbMix, "Reverb Mix")
STR(ParamReverbShimmerPitch, "Reverb Shimmer Pitch")
STR(ParamReverbShimmer, "Reverb Shimmer")
STR(ParamPhaserRate, "Phaser Rate")
STR(ParamPhaserDepth, "Phaser Depth")
STR(ParamPhaserCenter, "Phaser Center")
STR(ParamPhaserFeedback, "Phaser Feedback")
STR(ParamPhaserStages, "Phaser Stages")
STR(ParamPhaserMix, "Phaser Mix")
STR(ParamPhaserStereo, "Phaser Stereo")
STR(ParamPhaserNegative, "Phaser Negative")
STR(ParamPhaserSync, "Phaser Sync")
STR(ParamPhaserDivision, "Phaser Division")
STR(ParamDisperserFreq, "Phase Disp Freq")
STR(ParamDisperserStages, "Phase Disp Stages")
STR(ParamDisperserPinch, "Phase Disp Pinch")
STR(ParamDisperserMix, "Phase Disp Mix")
STR(ParamWidenerSize, "Widener Size")
STR(ParamWidenerWidth, "Widener Width")
STR(ParamWidenerHighPass, "Widener High Pass")
STR(ParamWidenerMix, "Widener Mix")
STR(ParamFlangerRate, "Flanger Rate")
STR(ParamFlangerDepth, "Flanger Depth")
STR(ParamFlangerDelay, "Flanger Delay")
STR(ParamFlangerFeedback, "Flanger Feedback")
STR(ParamFlangerMix, "Flanger Mix")
STR(ParamFlangerSync, "Flanger Sync")
STR(ParamFlangerDivision, "Flanger Division")
STR(ParamFlangerStereo, "Flanger Stereo")
STR(ParamFlangerNegative, "Flanger Negative")
STR(ParamAmpModel, "Amp Model")
STR(ParamAmpGain, "Amp Gain")
STR(ParamAmpBass, "Amp Bass")
STR(ParamAmpMid, "Amp Mid")
STR(ParamAmpTreble, "Amp Treble")
STR(ParamAmpPresence, "Amp Presence")
STR(ParamAmpVolume, "Amp Volume")
STR(ParamAmpOutput, "Amp Output")
STR(ParamAmpMix, "Amp Mix")
STR(ParamAmpStereo, "Amp Stereo")
STR(ParamAmpCabOn, "Amp Cabinet")
STR(ParamAmpCab, "Amp Cabinet Type")
STR(ParamAmpMic, "Amp Mic")
STR(ParamAmpPosition, "Amp Mic Position")
STR(ParamAmpDistance, "Amp Mic Distance")
STR(ParamAmpOversampling, "Amp Oversampling")
STR(ParamUtilityGain, "Utility Gain")
STR(ParamUtilityPan, "Utility Pan")
STR(ParamUtilityWidth, "Utility Width")
STR(ParamUtilityMonoBass, "Utility Mono Bass")
STR(ParamChorusRate, "Chorus Rate")
STR(ParamChorusDepth, "Chorus Depth")
STR(ParamChorusDelay, "Chorus Delay")
STR(ParamChorusFeedback, "Chorus Feedback")
STR(ParamChorusSpread, "Chorus Spread")
STR(ParamChorusMix, "Chorus Mix")
STR(ParamChorusVoices, "Chorus Voices")
STR(ParamShiftRange, "Shift Range")
STR(ParamShiftAmount, "Freq Shift")
STR(ParamShiftSpread, "Shift Spread")
STR(ParamShiftFeedback, "Shift Feedback")
STR(ParamShiftMix, "Shift Mix")

// A modulation slot's bypass, as the editor names it: the source, then the target it modulates.
STR(ParamNameBypass, "{1} bypass")
STR(ParamNameBypassOn, "{1} bypass on {2}")

// --- The host's remote control pages, and the plugin's ports

STR(PageMix, "Mix")
STR(PageMacros, "Macros")
STR(PageGlobal, "Global")
STR(PagePlay, "Play")
STR(PageOutputs, "Outputs")
STR(PageFilterLinks, "Filter links")
STR(PageShaperLoops, "Shapers into shapers")
STR(PageModulates, "{1} modulates")
STR(PageInto, "Into {1}")
STR(PortOutput, "Output")
STR(PortNotes, "Notes")

// --- VST3 hosts: the params MIDI controllers come in as, and the note expressions a note takes

STR(MidiCcParam, "MIDI CC {1} Ch {2}") // a controller's number, then its channel (1..16)
STR(MidiAftertouchParam, "MIDI Aftertouch Ch {1}")
STR(MidiPitchBendParam, "MIDI Pitch Bend Ch {1}")
STR(ExprVolume, "Volume")
STR(ExprPan, "Pan")
STR(ExprTuning, "Tuning")
STR(ExprBrightness, "Brightness")
STR(ExprPressure, "Pressure")

// --- Sources, presets and themes by name

STR(SourceNameLfo, "LFO {1}") // numbered by type in the order the sources show
STR(SourceNameEnv, "Env {1}")
STR(SourceNameRandom, "Random {1}")
STR(SourceNameMidi, "MIDI {1}")
STR(SourceNameCurve, "Curve {1}")
STR(SourceFoldedRandom, "RND {1}") // a Random source's name down its folded card, which "Random 3" doesn't fit
STR(SourceFoldedCurve, "CRV {1}")  // and a Curve source's, which "Curve 3" doesn't
STR(SourceNameNote, "Note")
STR(SourceNameWheel, "Mod Wheel")
STR(SourceNameVelocity, "Velocity")
STR(SourceNameBend, "Pitch Bend")
STR(MacroDefaultName, "Macro {1}") // until it is given a name of its own
STR(PresetInit, "- Init -") // the preset bar, with no preset loaded

STR(ThemeMidnight, "Midnight")
STR(ThemeLight, "Light")
STR(ThemeSpectrum, "Spectrum")
STR(ThemePorcelain, "Porcelain")

// --- The editor: headings, tabs and labels

STR(HeadingPlaying, "PLAYING")
STR(HeadingBend, "BEND")
STR(HeadingMod, "MOD")
STR(TempoName, "Tempo")
STR(ViewRouting, "ROUTING")
STR(ViewMatrix, "MATRIX")
STR(ViewFx, "FX")
STR(ViewMods, "MODS") // every modulation, as a table
// The button at the right of the view tabs, with ROUTING and MATRIX, and the panel of classic FM algorithms it opens
STR(ButtonAlgorithm, "ALGORITHM")
STR(AlgorithmBadgeOps, "{1}-OP {2}") // on the button: the algorithm of fewer than six operators the operators form
STR(AlgorithmOps, "Operators") // before the panel's switch between the sets: 6 | 4 | 3 | 2, how many they use
STR(AlgorithmHint, "Sets the operators' routes in one undo step. Filters and shapers stay as they are.")
STR(AlgorithmName, "Algorithm {1}") // the panel's foot, for the thumbnail under the mouse, then what follows
STR(AlgorithmCarriers, "{1} carriers")
STR(AlgorithmCarrierOne, "1 carrier")
STR(AlgorithmFeedback, "feedback on {1}") // an operator's letter
STR(AlgorithmFeedbackLoop, "feedback from {1} into {2}")
STR(AlgorithmEscape, "Esc closes")
STR(ModListCount, "{1} modulations") // in a pill's list, after the name of what they modulate
STR(ModListCountOne, "1 modulation")
STR(TabUnison, "UNISON") // an operator's page: what its lower half shows
STR(TabSpectrum, "SPECTRUM") // a noise operator's, in Unison's place
STR(TabEnvelope, "ENVELOPE")
STR(TabScope, "SCOPE")
STR(TabInputs, "INPUTS") // the routes into the operator, each with its mode
STR(TabWarp, "WARP") // its scope, warp and hard sync
STR(HeadingModulation, "MODULATION")
STR(HeadingKeyboard, "KEYBOARD")
STR(BusOut, "OUT") // the output, in the graph and the matrix
STR(HeadingHowToRead, "LEGEND")
STR(HeadingInputs, "INPUTS")
STR(HeadingLevel, "LEVEL")
STR(HeadingPhase, "PHASE")
STR(HeadingGrid, "GRID")
STR(LabelNote, "Note") // under the note source's display
STR(LabelVelocitySource, "Velocity") // under the velocity source's display

// The effect cards' titles.
STR(FxTitleShaper, "SHAPER")
STR(FxTitleDelay, "DELAY")
STR(FxTitleDjEq, "DJ EQ")
STR(FxTitleParamEq, "PARAM EQ")
STR(FxTitleFilter, "FILTER")
STR(FxTitleCompressor, "COMPRESSOR")
STR(FxTitleMultiComp, "3-BAND") // retired, never shown
STR(FxTitleMultiDyn, "3-BAND DYNAMICS")
STR(FxTitleReverb, "REVERB")
STR(FxTitlePhaser, "PHASER")
STR(FxTitleFlanger, "FLANGER")
STR(FxTitleFreqShifter, "FREQ SHIFT")
STR(FxTitleDisperser, "PHASE DISP")
STR(FxTitleWidener, "WIDENER")
STR(FxTitleAmp, "AMP")
STR(FxTitleUtility, "UTILITY")
STR(FxTitleChorus, "CHORUS")

// The big editors' titles.
STR(TitleShape, "{1} SHAPE")
STR(TitleFxShaperCurve, "FX {1} SHAPER CURVE")
STR(TitleShaperCurve, "SHAPER {1} CURVE")
STR(TitleNoteCurve, "NOTE CURVE")
STR(TitleVelocityCurve, "VELOCITY CURVE")
STR(TitlePartials, "OP {1} PARTIALS")
STR(AxisInput, "input →")
STR(AxisOutput, "output")
STR(AxisOut, "out")
STR(AxisVelocity, "velocity →")

// Knob labels.
STR(LabelVolume, "Volume")
STR(LabelFmAmount, "FM Amount")
STR(LabelPitch, "Pitch")
STR(LabelPan, "Pan")
STR(LabelGlide, "Glide")
STR(LabelBend, "Bend")
STR(LabelVoices, "Voices")
STR(LabelLow, "Low")
STR(LabelMid, "Mid")
STR(LabelHigh, "High")
STR(LabelDrive, "Drive")
STR(LabelOutput, "Output")
STR(LabelMix, "Mix")
STR(LabelAmount, "Amount")
STR(LabelTime, "Time")
STR(LabelTimeL, "Time L")
STR(LabelTimeR, "Time R")
STR(LabelFeedback, "Feedback")
STR(LabelTone, "Tone")
STR(LabelWidth, "Width")
STR(LabelQ, "Q")
STR(LabelCutoff, "Cutoff")
STR(LabelReso, "Reso")
STR(LabelResonance, "Resonance")
STR(LabelKeyTrack, "Key Track")
STR(LabelThreshold, "Threshold")
STR(LabelRatio, "Ratio")
STR(LabelKnee, "Knee")
STR(LabelAttack, "Attack")
STR(LabelHold, "Hold")
STR(LabelDecay, "Decay")
STR(LabelSustain, "Sustain")
STR(LabelRelease, "Release")
STR(LabelMakeup, "Makeup")
STR(LabelGain, "Gain")
STR(LabelLowX, "Low X")
STR(LabelCrossover, "Crossover") // a split's knob
STR(LabelCrossLow, "Low X") // a three-band split's knob for its low crossover
STR(LabelCrossHigh, "High X") // and for its high one
STR(LabelHighX, "High X")
STR(LabelSize, "Size")
STR(LabelPreDelay, "Pre-Delay")
STR(LabelDamp, "Damp")
STR(LabelShimmer, "Shimmer")
STR(LabelRate, "Rate")
STR(LabelDepth, "Depth")
STR(LabelCenter, "Center")
STR(LabelStages, "Stages")
STR(LabelDelay, "Delay")
STR(LabelRange, "Range")
STR(LabelAbove, "Above") // the three-band dynamics' thresholds and ratios: downward above, upward below
STR(LabelDown, "Down")
STR(LabelBelow, "Below")
STR(LabelUp, "Up")
STR(LabelShift, "Shift")
STR(LabelSpread, "Spread")
STR(LabelFreq, "Freq")
STR(LabelPinch, "Pinch")
STR(LabelHighPass, "High Pass")
STR(LabelMonoBass, "Mono Bass")
STR(LabelSlope, "Slope")
STR(LabelMult, "Mult") // a fixed operator's knob in Fine's place, Freq in Ratio's
STR(LabelDetune, "Detune")
STR(LabelUnison, "Unison")
STR(LabelRndPhase, "Rnd Phase")
STR(LabelBlend, "Blend") // the unison copies' Blend: the outer ones turned down
STR(LabelSmooth, "Smooth")
STR(LabelChaos, "Chaos")
STR(LabelPhase, "Phase")
STR(LabelSlew, "Slew")
STR(LabelCc, "CC")
STR(LabelBass, "Bass")
STR(LabelTreble, "Treble")
STR(LabelPresence, "Presence")
STR(LabelPosition, "Position")
STR(LabelDistance, "Distance")

// Switches and buttons.
STR(SwitchPoly, "POLY")
STR(SwitchMono, "MONO")
STR(SwitchLegato, "LEGATO")
STR(SwitchAlways, "ALWAYS") // the glide's
STR(SwitchHardSync, "NOTE") // an operator's note sync switch: hard sync to the note (not the Sync warp)
STR(CaptionNoteSync, "NOTE SYNC") // on the WARP display, while the operator is synced to the note
STR(SwitchStereo, "STEREO")
STR(SwitchPoints, "Points") // what a click on an LFO's shape does
STR(SwitchSteps, "Steps")
STR(SwitchSnap, "Snap") // an LFO's shape points to the grid
STR(SwitchHard, "HARD") // the shaper effect's first three curves
STR(SwitchSoft, "SOFT")
STR(SwitchAsym, "ASYM")
STR(SwitchSolo, "Solo") // a join's card: solo the band above it
STR(SoloMark, "S") // on a join node while one of its bands is soloed
STR(SwitchRms, "RMS") // the three-band dynamics' detector: RMS, or peaks while off
STR(BandButtonLow, "LOW") // the three-band dynamics' bands, and the DJ EQ's display
STR(BandButtonMid, "MID")
STR(BandButtonHigh, "HIGH")
STR(ButtonAddEffect, "ADD EFFECT")
STR(ButtonClearFx, "Remove All") // the confirm dialog's button that removes every effect
STR(ButtonAddSource, "ADD SOURCE") // the dashed card after the sources: opens the list of source types
STR(ButtonEffect, "EFFECT") // + EFFECT and + SOURCE, after a drawn plus
STR(ButtonSource, "SOURCE")
STR(ButtonOpenFolder, "OPEN FOLDER")
STR(ButtonCancel, "Cancel")
STR(ButtonInit, "Discard")
STR(ButtonLoad, "Load")
STR(ButtonOverwrite, "Overwrite")

// --- The editor: hints and notes along the panels

STR(HintRouting, "drag edges to connect")
STR(HintMatrix, "rows feed columns  ·  drag to adjust  ·  double-click to toggle")
STR(HintFx, "drag a title to reorder")
STR(HintFxGraph, "click node: edit  ·  drag node: move  ·  click wire: insert")
STR(HintFxInspector, "Click a node to edit it") // the graph's inspector, with no node to show
// A split's controls: the tabs that pick the band it shows, and its container's dashed card adding to that band.
STR(TabBandLow, "Low")
STR(TabBandHigh, "High")
STR(TabBandMid, "Mid") // a three-band split's middle band
STR(ButtonAddToBand, "Add to") // over the band's name (TabBandLow or TabBandHigh)
// What the note source's and velocity's curves show:
STR(HintNoteAxes, "across: notes from Low to High  ·  up: output")
STR(HintVelocityAxes, "across: velocity  ·  up: output")
// A shape or curve, by how a click works on it and whether its points snap:
STR(HintShape, "{1}  ·  dots: bend  ·  Ctrl: {2}")
STR(HintShapePoints, "double-click: add/delete  ·  Shift+click: step")
STR(HintShapeSteps, "click: step  ·  Shift+click: points")
STR(HintCtrlSnap, "snap")
STR(HintCtrlFree, "free")
STR(HintPartials, "drag to draw  ·  double-click to silence")
STR(HintNoSources, "No modulators yet.")

// The matrix's legend.
STR(LegendFm, "FM between operators")
STR(LegendFeedback, "Diagonal: feedback")
STR(LegendFilter, "Filter input or output")
STR(LegendShaper, "Shaper input or output")
STR(LegendOut, "Straight to the output")
STR(MatrixClick, "Click a name to open it in the inspector.")
STR(MatrixClickThen, "Click again to toggle.")

// The settings dialog.
STR(SettingsTitle, "Settings")
STR(SettingsOversampling, "OVERSAMPLING")
STR(SettingsOversamplingWhat, "Runs the voices at a multiple of the sample rate.")
STR(SettingsOversamplingWhy, "Higher values reduce aliasing but use more CPU.")
STR(SettingsOversamplingSaved, "Not stored in presets.")
STR(SettingsMpe, "MPE")
STR(SettingsMpeBend, "BEND") // the member channels' bend range, beside its buttons
STR(SettingsMpeRange, "{1} st") // a bend range in semitones, on its button
STR(SettingsMpeWhat, "Requires MPE in the host,")
STR(SettingsMpeWhy, "unless it sends CLAP note expressions.")
STR(SettingsMpeSaved, "Not stored in presets.")
STR(SettingsLook, "LOOK")
STR(SettingsHue, "HUE") // the theme's main color, beside its slider
STR(SettingsHueDefault, "Default") // the hue slider left at the theme's own color, after it
STR(SettingsGlow, "GLOW")
STR(SettingsLinks, "LINES") // how strongly a source's dashed lines to its targets are drawn, beside its slider
STR(SettingsHandles, "HANDLES") // the depth handles' size, beside its buttons
STR(SettingsHandlesSmall, "Small")
STR(SettingsHandlesMedium, "Medium")
STR(SettingsHandlesLarge, "Large")
STR(SettingsLookSaved,"Applies to all instances.")
STR(SettingsVersion, "FMVibe {1}") // the version number, at the bottom
STR(SettingsLicense, "Released under the MIT license")

// The preset browser, and the question before a preset replaces a changed patch.
STR(PresetsTitle, "Presets")
STR(PresetsCountOne, "1 in the folder")
STR(PresetsCount, "{1} in the folder")
STR(PresetsNone, "No presets yet.")
STR(PresetsNoneHint, "Save a preset with the disk icon, or place .fmv files into the presets folder.")
STR(PresetsSearchHint, "Type to search")
STR(PresetsMatching, "{1} of {2} match") // presets that match the search, of all in the folder
STR(PresetsNoMatch, "No presets match \"{1}\"")
STR(ConfirmTitle, "Discard the current patch?")
STR(ConfirmLoadPreset, "Loading {1} replaces every setting.")
STR(ConfirmInit, "Resets all settings to defaults.")
STR(ConfirmSaveFirst, "Unsaved changes will be lost.")
STR(ConfirmOverwriteTitle, "Overwrite \"{1}\"?")
STR(ConfirmOverwrite, "Replaces the preset file with the current patch.")
STR(ConfirmFxClearTitle, "Really remove all FX?")
STR(ConfirmFxClear, "All effects are removed.")
STR(ConfirmMidBandTitle, "Change to two bands?") // a three-band split switched to two, its middle band holding effects
STR(ConfirmMidBand, "The mid band holds {1} effects. Where should they go?")
STR(ConfirmMidBandOne, "The mid band holds an effect. Where should it go?")
STR(ButtonMidToLow, "Onto Low") // the middle band's effects onto the low band's end
STR(ButtonMidToHigh, "Onto High") // onto the high band's start
STR(ButtonMidRemove, "Remove")
STR(ConfirmSplitRemoveTitle, "Remove the split?") // a split removed while its bands hold effects
STR(ConfirmSplitRemove, "Keep its {1} effects on the main line?")
STR(ConfirmSplitRemoveOne, "Keep its effect on the main line?")
STR(ButtonSplitKeep, "Keep") // the split goes, its bands' effects staying on the main line
STR(ButtonSplitDelete, "Delete") // the split goes with its bands' effects

// Readouts in the displays, with the numbers filled in.
STR(CaptionShapePoint, "point {1}  ·  {2} % of the cycle  ·  {3}")
// In a widened LFO card, under its shape: the point under the mouse, its place in the cycle and its level.
STR(CaptionCardPointNumber, "#{1}")
STR(CaptionCardPoint, "{1} %  ·  {2}")
STR(HintCardPoint, "Hover a point")
STR(CaptionCurvePoint, "point {1}  ·  in {2}  ·  out {3}")
STR(CaptionNotePoint, "point {1}  ·  {2}  ·  out {3} %")
STR(CaptionVelocityPoint, "point {1}  ·  velocity {2}  ·  out {3} %")
STR(CaptionPartial, "partial {1}  ·  level {2} dB  ·  phase {3}°")
STR(CaptionPartialSilent, "partial {1}  ·  level none  ·  phase {2}°")
STR(CaptionReduction, "GR {1} dB")
STR(CaptionBandKnobs, "{1} BAND") // over the knobs a three-band dynamics band has of its own, with its name
STR(CaptionAllBands, "ALL BANDS") // the same, linked
STR(CaptionPreDelay, "{1}  ·  pre {2}")
STR(CaptionShimmer, "{1}  ·  shift {2}")
STR(CaptionStages, "{1} stages")
STR(CaptionVoices, "{1} voices")
STR(CaptionDispersion, "{1}  ·  up to {2} ms")
STR(CaptionWidener, "HP {1}  ·  {2}–{3} ms")
STR(CaptionAmpDrive, "drive {1} dB") // how far the amp's stages saturate now
STR(CaptionAmpMic, "{1}  ·  {2}") // the amp's mic and its distance
STR(CaptionUtility, "{1}  ·  {2}") // the utility's gain and pan
STR(CaptionUtilityMonoBass, "mono < {1}") // on the utility's Mono Bass line: below it, the sound is mono
STR(CaptionShiftOf, "{1}  ·  of {2}")
STR(CaptionHold, "hold {1}")
STR(CaptionResonance, "{1}  ·  res {2}")
STR(CaptionEnvCurve, "{1} curve  ·  {2}") // an envelope segment's knob label, then its curve
STR(CaptionFeedback, "fb {1}") // an operator's feedback on its wire label
STR(CaptionBypassed, "(off)") // after a bypassed modulation in a list
STR(NoiseWhiteCaption, "white")
STR(NoisePinkCaption, "pink")
STR(NoiseBrownCaption, "brown")
STR(TipMoreWaves, "More waves")
STR(TipFixedOff, "Fixed pitch at Freq × Mult") // the keyboard switch, tracking the keys
STR(TipHardSync, "Note sync: restart the wave with each cycle of the note") // an operator's NOTE switch
STR(TipWarpOff, "Off") // an operator's warps, its WARP tab's buttons
STR(TipWarpBend, "Bend")
STR(TipWarpSqueeze, "Squeeze")
STR(TipWarpPulse, "Pulse")
STR(TipWarpSteps, "Steps")
STR(TipWarpSync, "Sync")
STR(TipUniCurveLinear, "Copies spread evenly in pitch") // the UNISON tab's buttons
STR(TipUniCurveCenter, "Copies bunched about the pitch")
STR(TipUniCurveSupersaw, "Supersaw spacing")
STR(TipUniCurveRandom, "Random detune")
STR(TipUniPanRamp, "Left to right as the pitch rises")
STR(TipUniPanAlternate, "Copies next in pitch on opposite sides")
STR(TipUniPanRandom, "Random places")
STR(SummarySync, "note sync") // on an operator's summary line, while it is synced to the note
STR(TipFixedOn, "Fixed pitch on. Click to track keys")

// The inspector's summary line, after the node's letter and its wave, filter type or curve; an operator's ratio
// goes in as a bare number. Keep it short: the line has room for about 38 characters.
STR(SummaryUnison, "×{1}") // the operator's unison copies
STR(SummaryTo, "to {1}")
STR(SummaryOut, "out")
STR(SummaryNoInput, "no input yet")
STR(SummaryFrom, "from {1}")
STR(SummaryMods, "mods {1}")

// A node within a sentence.
STR(NodeOperator, "operator {1}")
STR(NodeFilter, "filter {1}")
STR(NodeShaper, "shaper {1}")
STR(NodeFxIn, "IN") // the effects graph's input, the voices
STR(NodeFxOut, "OUT") // its output
STR(NodeSplit, "SPLIT {1}") // a split in the effects graph, by number
STR(NodeJoin, "JOIN {1}") // a join in the effects graph, by number, as its split's

// --- The editor: tooltips

STR(TipRenameMacro, "Double-click to rename")
STR(TipModPill, "{1} more. Click to list") // the "+n" pill in place of depth handles that don't fit
STR(TipNoteDisplay, "Edit note curve")
STR(TipVelocityDisplay, "Edit velocity curve")
STR(TipWireEnd, "Drag to reroute")
STR(TipWireStart, "Drag onto another node to change source")
STR(TipWire, "Drag to bend, double-click resets")
STR(TipFxChain, "Chain View")
STR(TipFxGraph, "Graph View")
STR(TipFxNode, "Click to show its card, drag to move it")
STR(TipFxSlot, "Add an effect or a split here")
STR(TipFxSlotBand, "Add an effect to this band")
STR(TipFxGap, "Add an effect or a split here") // the middle of a wire in the effects graph
STR(TipFxGapBand, "Add an effect to this band here")
STR(TipFxClear, "Remove all effects")
STR(TipRemoveSplit, "Remove split")
STR(TipRemoveJoin, "Remove join")
STR(TipRouteMode, "{1}. Click to pick how it modulates") // the mode part of a wire label's pill
STR(TipRemoveRoute, "Remove route") // the x at the right of a wire or OUT label's pill
STR(TipRoutePhase, "Phase: adds to where the wave is read")
STR(TipRouteLinearFm, "Linear FM: adds to the frequency, through zero")
STR(TipRouteExpFm, "Exp FM: moves the pitch by octaves")
STR(TipRouteRing, "Ring: multiplies the output, 50 % is AM")
STR(TipPort, "Drag to a node or OUT to connect")
STR(TipFxInsert, "Add an effect or a split here") // the arrow between two effect cards on the main line
STR(TipSplitGrip, "Drag to move the split with its bands") // a split's container's title row
STR(TipSplitBand, "Show this band's effects") // a split's band tab
STR(TipSplitSolo, "Solo this band") // the S over a split's band level
STR(TipSplitTwo, "Split into two bands") // a split's BANDS switch
STR(TipSplitThree, "Split into three bands")
STR(TipWheelHandle, "Drag to assign mod wheel")
STR(TipNoteHandle, "Drag to assign note pitch modulation")
STR(TipVelocityHandle, "Drag to assign velocity")
STR(TipBendHandle, "Drag to assign pitch bend")
STR(TipMacroHandle, "Drag to assign {1}")
STR(TipSourceHandle, "Drag to assign {1}") // an LFO's or an envelope's handle, with its name
STR(TipShowNode, "Show {1}")
STR(TipSwitchOnAndShow, "Turn on and show {1}")
STR(TipSwitchNodeOff, "Turn off {1}")
STR(TipSwitchNodeOn, "Turn on {1}")
STR(TipNodeGhost, "{1} is off. Click to turn on")
STR(TipPreviousPreset, "Previous preset")
STR(TipNextPreset, "Next preset")
STR(TipPresetChanged, "Modified  ·  browse presets")
STR(TipPresetName, "Browse presets")
STR(TipTempo, "Tempo for synced LFOs and effects. Drag, scroll or double-click to type")
STR(TipOpenFolder, "Open preset folder")
STR(TipInit, "Reset all settings")
STR(TipLoad, "Load preset")
STR(TipSave, "Save changes to {1}")
STR(TipSaveUnchanged, "No changes to save")
STR(TipSaveNoFile, "No preset loaded. Use Save As...")
STR(TipSaveAs, "Save as a new preset")
STR(TipSettings, "Settings")
STR(TipBypassed, "Bypassed. Click to resume")
STR(InputsNone, "Nothing modulates {1} yet.") // an operator's INPUTS tab with no route into it
STR(InputsFeedback, "{1} feedback  ·  phase only")
STR(TipBypass, "Bypass modulation")
STR(TipSourceUnipolar, "Unipolar: one way. Click for bipolar") // a MODS row's switch
STR(TipSourceBipolar, "Bipolar: both ways. Click for unipolar")
STR(TipResetNoteCurve, "Reset to full key tracking")
STR(TipResetVelocityCurve, "Reset to linear")
STR(TipResetCurve, "Reset to S-curve")
STR(TipResetShape, "Reset to triangle")
STR(TipResetPartials, "Reset to sine")
STR(TipRandomPartials, "Randomize partials") // random levels falling 3 dB an octave, random phases
STR(TipEditPartials, "Edit partials") // a Custom wave's edit button, by the keyboard switch
STR(TipEditCurve, "Open curve editor") // a drawn shaper curve's edit button
STR(TipClosePartials, "Close partial editor")
STR(TipDone, "Done")
STR(TipShapeCollapse, "Collapse")
STR(TipShapeExpand, "Open large editor")
STR(TipShapeWiden, "Edit shape") // a Custom LFO's normal card: widens it to edit its shape
STR(TipShapeInvert, "Invert")
STR(TipShapeReverse, "Reverse")
STR(TipShapeShiftLeft, "Shift left")
STR(TipShapeShiftRight, "Shift right")
STR(TipShapeCopy, "Copy shape")
STR(TipShapePaste, "Paste copied shape")
STR(TipShapePoints, "Double click to add and move points")
STR(TipShapeSteps, "Click to draw steps, Shift+click for points")
STR(TipShapeSnap, "Snap to grid")
STR(TipGridAcross, "Horizontal grid (Ctrl to snap)")
STR(TipGridUpDown, "Vertical grid (Ctrl to snap)")
STR(TipFxRemove, "Remove effect")
STR(TipSourceRemove, "Remove source and its modulation")
STR(TipNodePower, "Turn off")
STR(TipSourcePower, "Turn off")
STR(TipPoly, "One voice per note")
STR(TipMono, "One voice at a time")
STR(TipLegato, "Overlapping notes don't retrigger envelopes")
STR(TipFxOn, "Click to turn off, drag to move")
STR(TipFxOff, "Click to turn on, drag to move")
STR(TipLfoSync, "Sync rate to tempo")
STR(TipSlope, "Filter slope")
STR(TipSlopeBand, "Filter slope")
STR(TipRetrigger, "Restart with each note") // an LFO's clock modes
STR(TipFree, "Free")
STR(TipOneShot, "One shot")
STR(TipAnchor, "Anchor to song position") // an LFO's switch: its phase follows the host's transport
STR(TipUnipolar, "Unipolar")
STR(TipBipolar, "Bipolar") // a Random card's switch, beside Unipolar
STR(TipDelaySync, "Sync times to tempo")
STR(TipSweepSync, "Sync sweep to tempo")
STR(TipChorusVoices, "Number of voices")
STR(TipPingPong, "Alternate echoes between sides")
STR(TipDelayLink, "Left time for both sides")
STR(TipBandLink, "Edit all bands")
STR(TipMcBand, "{1} band settings") // a three-band dynamics band's row, with the band's name
STR(TipMcBandLinked, "{1} band settings, linked") // the band's name; its edits go to every band
STR(TipDownThreshold, "Downward threshold")
STR(TipDownRatio, "Downward ratio")
STR(TipUpThreshold, "Upward threshold")
STR(TipUpRatio, "Upward ratio")
STR(TipUpRange, "Upward gain limit")
STR(TipRms, "RMS detection (off: peaks)")
STR(TipDownGrip, "Downward threshold · Alt+drag: ratio")
STR(TipUpGrip, "Upward threshold · Alt+drag: ratio")
STR(TipDownZone, "Downward ratio") // the stretch right of the Above threshold's line
STR(TipUpZone, "Upward ratio")       // and left of the Below threshold's
STR(TipPartialCount, "First {1} partials only")
STR(TipStereo, "Offset right sweep by 90°")
STR(TipPositive, "Positive: wet added to dry")
STR(TipNegative, "Negative: wet and feedback inverted")
STR(TipShaperLink, "Compensate level for drive")
STR(TipShaperSymmetric, "Mirror right half onto left")
STR(TipClipHard, "Hard clipping")
STR(TipClipSoft, "Soft clipping")
STR(TipClipAsym, "Asymmetric clipping")
STR(TipGlideAlways, "Glide between all notes")
STR(TipMidiCc, "MIDI CC, chosen with the CC knob")
STR(TipMidiPressure, "Aftertouch, per note or channel")
STR(TipMidiSlide, "Per-note slide (MPE CC 74)")

// --- The editor: right-click menus

STR(MenuDuplicate, "Duplicate")
STR(MenuSplitBands, "2-band split") // the list of effects on the main line, in its SPLIT group (MenuFxSplit)
STR(MenuSplitThree, "3-band split") // under it: a split in three bands
// The list of effects' groups, each over its effects, in capitals as the target menu's headings.
STR(MenuFxDrive, "DRIVE") // the shaper and the amp
STR(MenuFxEqFilter, "EQ & FILTER")
STR(MenuFxDynamics, "DYNAMICS")
STR(MenuFxSpace, "SPACE") // the delay and the reverb
STR(MenuFxModulation, "MODULATION") // the phaser, the flanger, the chorus and the frequency shifter
STR(MenuFxStereo, "STEREO & PHASE") // the widener, the phase disperser and the utility
STR(MenuFxSplit, "SPLIT") // from the main line: a split in two bands and in three (MenuSplitBands, MenuSplitThree)
STR(MenuResetSettings, "Reset settings")
STR(MenuOversampling, "Oversampling {1}") // an amp's, {1} the factor ("4x"), the one in force checked
STR(MenuOtt, "OTT") // a 3-band dynamics' settings set after the well-known OTT multiband preset
STR(MenuRemove, "Remove")
STR(MenuMoveToBand, "Move to {1}") // an effect on a split's band moved to the end of another band of it, {1} its name
STR(MenuUnipolar, "Unipolar") // a source's, in the menus of its card, handle or knob
STR(MenuBipolar, "Bipolar")
STR(MenuResetBend, "Reset Bend") // a wire in the graph, back to how it bows to begin with
STR(MenuClearTargets, "Clear all {1} targets")
STR(MenuEditDepth, "Edit depth…")
STR(MenuBypass, "Bypass {1} modulation")
STR(MenuResume, "Resume {1} modulation")
STR(MenuRemoveModulation, "Remove {1} modulation")
STR(MenuRemoveAux, "Remove {1} aux") // the source scaling a modulation's depth
STR(MenuRemoveAll, "Remove all modulation")
STR(MenuDeletePoint, "Delete point")
STR(MenuStraighten, "Straighten")
STR(MenuSwitchOff, "Switch off {1}")
STR(MenuDisconnect, "Disconnect")
STR(MenuEditValue, "Edit value…")
STR(MenuResetDefault, "Reset to default")

// --- The editor: undo and redo, on their buttons' tooltips

STR(UndoNothing, "Nothing to undo")
STR(RedoNothing, "Nothing to redo")
STR(UndoParam, "Undo {1}")
STR(RedoParam, "Redo {1}")
STR(UndoLayout, "Undo layout change")
STR(RedoLayout, "Redo layout change")
STR(UndoChanges, "Undo {1} changes")
STR(RedoChanges, "Redo {1} changes")

// --- The editor: messages that flash up when something can't be done

STR(MsgAlreadyModulates, "{1} already modulates {2}")
STR(MsgSlotsFull, "{1}: all {2} slots are in use")
STR(MsgOwnKnob, "{1} can't modulate its own knob")
STR(MsgMacroLoop, "{1} moves {2} already: it can't modulate it back") // macros modulating each other in a loop
STR(MsgEmptyModsFull, "Up to {1} empty modulations allowed.")
STR(MsgRouteExists, "{1} already routes to {2}")
STR(MsgChainFull, "Only supports up to {1} effects in the chain")
STR(MsgSplitsFull, "Only supports up to {1} splits")
STR(MsgShapeFull, "Only supports up to {1} points per shape")
STR(MsgShapeCopied, "{1} shape copied")
STR(MsgNothingCopied, "Copy a shape first")
STR(MsgNoteCurveEnds, "End points can't be deleted")
STR(MsgCurveEnds, "End points can't be deleted")
STR(MsgCurveMiddle, "Symmetric curves keep their middle point")
STR(MsgFirstPoint, "First point can't be deleted")
STR(MsgTwoPoints, "A shape needs two points")
STR(MsgSourcesFull, "Only supports up to {1} sources")
STR(MsgPresetUnreadable, "Preset could not be read")
STR(MsgNoPresets, "Preset folder is empty")
STR(MsgPresetSaved, "Saved {1}")
STR(MsgPresetNotSaved, "Could not save preset")
STR(MsgBadValue, "Invalid value for {2}: \"{1}\"")

// --- The plugin's file dialogs

STR(DialogSavePreset, "Save preset")
STR(DialogLoadPreset, "Load preset")
STR(DialogPresetFiles, "FMVibe preset (*.fmv)")
STR(DialogAllFiles, "All files (*.*)")
STR(DialogWriteFailed, "Could not save the preset.")
STR(DialogNotPreset, "Not an FMVibe preset.")

// --- The browser demo, which has no files

STR(DemoNoFiles, "No file access in the demo. Click the preset name for factory presets.")
STR(DemoPresetFolder, "Built-in factory presets")

// --- The MODS view: every modulation as a row, and the cascading menus of its rows

STR(ModsHeadSource, "SOURCE")
STR(ModsHeadTarget, "TARGET")
STR(ModsHeadDepth, "DEPTH")
STR(ModsHeadAmount, "AMOUNT")
STR(ModsHeadAux, "AUX")
STR(ModsHeadCurve, "CURVE")
STR(ModsAddAux, "+ Aux") // a row without an aux: opens the menu of auxes
STR(ModsAddModulation, "+ Add modulation") // after the last row: adds an empty one
STR(CardNoModulations, "No modulations") // a source card's button, while the source modulates nothing
STR(ModsPickSource, "Source…") // an empty modulation's, until one is picked
STR(ModsPickTarget, "Target…")
STR(ModsCount, "{1} modulations")
STR(ModsCountOne, "1 modulation")
STR(ModsBypassed, "{1} bypassed")
STR(ModsAtZero, "{1} at 0 %")
STR(ModsEmpty, "{1} empty") // modulations without a source or a target yet
STR(HintNoMods, "No modulations yet. Drag a handle onto a knob to add one.")
STR(ModsFilterHint, "Filter by source or target")
STR(ModsMatching, "{1} of {2}") // the rows the filter shows, of all the modulations
STR(ModsNoMatch, "No modulation matches \"{1}\"")
STR(TipModsFilter, "Filter modulations")
STR(TipAlgorithm, "Set the operators to a classic FM algorithm")
STR(TipModsSource, "Change source")
STR(TipModsTarget, "Change target")
STR(TipModsPickSource, "Pick its source") // an empty modulation's
STR(TipModsPickTarget, "Pick its target")
STR(TipModsAux, "Scale depth by another source")
STR(TipModsAuxClear, "No aux")
STR(TipModsScale, "Amount: scales the depth")
STR(TipModsCurve, "Curve {1}: drag to bend, double-click to reset")
STR(CascadeLfos, "LFOs")
STR(CascadeEnvelopes, "Envelopes")
STR(CascadeNoteMidi, "Note & MIDI") // Random and MIDI sources
STR(CascadeMacros, "Macros")
STR(CascadeKeyboard, "Keyboard") // the note source, velocity and the mod wheel
STR(CascadeGlobal, "Global")
STR(CascadeOperators, "Operators")
STR(CascadeMatrix, "Matrix")
STR(CascadeFilters, "Filters")
STR(CascadeShapers, "Shapers")
STR(CascadeEffects, "Effects")
STR(CascadeModulation, "Modulation")
STR(CascadePitch, "PITCH")
STR(CascadeLevel, "LEVEL")
STR(CascadePhase, "PHASE")
STR(CascadeEnvelope, "ENVELOPE")
STR(CascadeUnison, "UNISON")
STR(CascadeNoise, "NOISE")
STR(CascadeFields, "SETTINGS") // a source's own knobs, before the depths of its modulations
STR(CascadeDepths, "DEPTHS")
STR(CascadeFeedback, "Feedback") // a node into itself, in the matrix's list
STR(CascadeAlready, "on it already") // a source that modulates the row's target already
STR(CascadeSlotsFull, "no slot free")
STR(CascadeItself, "itself") // a row's own source, which can't be its aux
STR(CascadeOwnKnob, "its own knob")
STR(CascadeLoop, "would loop") // a macro moved by the macro whose knob it would modulate
STR(CascadeOff, "off") // a switched-off operator
STR(CascadeSearchHint, "Search targets")
STR(CascadeNoMatch, "No target matches") // the target menu's search found nothing
STR(CascadeMore, "{1} more: type more to narrow") // matches past those the target menu lists
