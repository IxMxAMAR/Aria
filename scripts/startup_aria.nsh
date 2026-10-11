echo -off
#
# Aria - UEFI Shell startup script.
#
# Copy this to the root of your USB partition as startup.nsh. It locates
# ariaefi.efi (as placed by scripts\deploy_efi.bat) and runs it.
#
for %f in fs0 fs1 fs2 fs3 fs4 fs5
  if exist %f:\EFI\aria\ariaefi.efi then
    echo Running %f:\EFI\aria\ariaefi.efi
    %f:\EFI\aria\ariaefi.efi
    goto aria_done
  endif
endfor
echo "Aria: ariaefi.efi not found in \EFI\aria on fs0-fs5"
:aria_done
