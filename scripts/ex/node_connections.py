"""
Node connection and routing operations.
This example shows how to connect/disconnect nodes and check connections.
"""

import time

print("Node Connections Demo")
    
# Clear all existing connections
nodes_clear()
print("Cleared all connections")

# Test connections
test_connections = [
    (1, 30),
    (15, 45),
    (DAC0, 20),
    (GPIO_1, 25)
]

for node1, node2 in test_connections:
    oled_print("\nConnecting " + str(node1) + " to " + str(node2))
    
    # Connect nodes
    connect(node1, node2)
    
    # Check connection
    connected = is_connected(node1, node2)
    oled_print("Is connected: " + str(connected))
    
    time.sleep(0.5)
    
    # Disconnect
    disconnect(node1, node2)
    
    # Verify disconnection
    connected = is_connected(node1, node2)
    oled_print("Is connected: " + str(connected))
    
    time.sleep(0.5)

# Show final status
print("\nFinal status:")
print_bridges()

print("Node Connections complete!")
nodes_clear()

