@for /f "tokens=2*" %a in ('reg query "HKLM\System\CurrentControlSet\Control\Session Manager\Environment" /v Path') do set PATH=%b
