// The ids of the VST3 interfaces the plugin and its test host use. The VST3 SDK's headers declare them; one place
// in a module defines them (the base ones are in the SDK's coreiids.cpp).

#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstphysicalui.h>
#include <pluginterfaces/vst/ivstunits.h>

namespace Steinberg {

DEF_CLASS_IID(IPlugView)
DEF_CLASS_IID(IPlugFrame)

namespace Vst {

DEF_CLASS_IID(IComponent)
DEF_CLASS_IID(IAudioProcessor)
DEF_CLASS_IID(IProcessContextRequirements)
DEF_CLASS_IID(IEditController)
DEF_CLASS_IID(IEditController2)
DEF_CLASS_IID(IComponentHandler)
DEF_CLASS_IID(IComponentHandler2)
DEF_CLASS_IID(IMidiMapping)
DEF_CLASS_IID(INoteExpressionController)
DEF_CLASS_IID(INoteExpressionPhysicalUIMapping)
DEF_CLASS_IID(IUnitInfo)
DEF_CLASS_IID(IEventList)
DEF_CLASS_IID(IParameterChanges)
DEF_CLASS_IID(IParamValueQueue)
DEF_CLASS_IID(IHostApplication)
DEF_CLASS_IID(IConnectionPoint)

} // namespace Vst
} // namespace Steinberg
