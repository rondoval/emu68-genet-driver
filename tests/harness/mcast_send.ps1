# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0+
# Puts a small datagram on each of GenetTest MCAST's three multicast groups
# every 50 ms, from the PC's LAN interface, for its phases. WSL's NAT cannot
# reach LAN multicast, so the suite runs this through powershell.exe
# (harness/peers.py).
#
#   powershell.exe -NoProfile -File mcast_send.ps1 -Interface <PC LAN address> [-Seconds 40]
param(
    [Parameter(Mandatory = $true)][string]$Interface,
    [int]$Seconds = 40,
    [int]$Port = 5004
)
$groups = '239.255.77.3', '239.255.77.17', '239.255.78.1'
$udp = New-Object System.Net.Sockets.UdpClient
$ip = [System.Net.Sockets.SocketOptionLevel]::IP
$udp.Client.SetSocketOption($ip, [System.Net.Sockets.SocketOptionName]::MulticastInterface,
    [System.Net.IPAddress]::Parse($Interface).GetAddressBytes())
$udp.Client.SetSocketOption($ip, [System.Net.Sockets.SocketOptionName]::MulticastTimeToLive, 1)
$payload = [System.Text.Encoding]::ASCII.GetBytes('GenetTest')
$end = (Get-Date).AddSeconds($Seconds)
$sent = 0
while ((Get-Date) -lt $end) {
    foreach ($group in $groups) {
        [void]$udp.Send($payload, $payload.Length, $group, $Port)
        $sent++
    }
    Start-Sleep -Milliseconds 50
}
$udp.Close()
"sent $sent datagrams"
