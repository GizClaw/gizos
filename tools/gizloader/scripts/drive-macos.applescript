on run arguments
  set operation to item 1 of arguments
  tell application "System Events"
    tell process "gizloader"
      if operation is "quit" then
        set frontmost to true
        keystroke "q" using command down
        return "Closed Gizloader"
      end if
      if operation is "scan" or operation is "logs" then
        if operation is "scan" then
          set label to "扫描串口"
        else
          set label to "串口日志"
        end if
        set itemControls to entire contents of window "Gizloader"
        repeat with itemControl in itemControls
          if class of itemControl is button then
            if name of itemControl is label then
              click itemControl
              return "Clicked " & label
            end if
          end if
        end repeat
        error "Missing Gizloader itemControl: " & label
      end if
      if operation is "snapshot" then
        set labels to {}
        repeat with itemControl in entire contents of window "Gizloader"
          if class of itemControl is static text then
            try
              set end of labels to value of itemControl
            end try
          end if
        end repeat
        set AppleScript's text item delimiters to linefeed
        return labels as text
      end if
      error "Expected scan, logs, snapshot or quit"
    end tell
  end tell
end run
