#ifndef GNINE_RUNTIME_INPUT_STATE_HPP
#define GNINE_RUNTIME_INPUT_STATE_HPP

namespace gnine
{
   struct RuntimeInputState
   {
      double keyUp;
      double keyDown;
      double keyLeft;
      double keyRight;
      double keyW;
      double keyA;
      double keyS;
      double keyD;
      double keySpace;
      double keyReturn;
      double keyEscape;
      double mouseX;
      double mouseY;
      double mouseLeft;
      double mouseRight;
      double mouseWheelY;
      double keyShift;
      double keyCtrl;
      double keyTab;
      double key0;
      double key1;
      double key2;
      double key3;
      double key4;
      double key5;
      double key6;
      double key7;
      double key8;
      double key9;
      bool quitRequested;

      RuntimeInputState()
         : keyUp(0.0), keyDown(0.0), keyLeft(0.0), keyRight(0.0),
           keyW(0.0), keyA(0.0), keyS(0.0), keyD(0.0),
           keySpace(0.0), keyReturn(0.0), keyEscape(0.0),
           mouseX(0.0), mouseY(0.0), mouseLeft(0.0), mouseRight(0.0),
           mouseWheelY(0.0),
           keyShift(0.0), keyCtrl(0.0), keyTab(0.0),
           key0(0.0), key1(0.0), key2(0.0), key3(0.0), key4(0.0),
           key5(0.0), key6(0.0), key7(0.0), key8(0.0), key9(0.0),
           quitRequested(false)
      {
      }
   };
}

#endif
