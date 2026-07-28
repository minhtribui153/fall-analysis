import asyncio
import time
import pandas as pd
from bleak import BleakScanner, BleakClient

DEVICE_NAME = "Nesso_N1_HAR"
CHARACTERISTIC_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"

# List to hold all the incoming rows of data
collected_data = []

# This function triggers automatically every time the ESP32 sends a new reading
def notification_handler(sender, data):
    # Decode the byte array to a string: "ax,ay,az,gx,gy,gz,class,prob"
    payload = data.decode('utf-8').strip()
    
    # Split the string by commas and add a timestamp
    row = [time.strftime("%H:%M:%S")] + payload.split(",")
    collected_data.append(row)
    print(f"Received: {payload}")

async def main():
    print(f"Scanning for {DEVICE_NAME}...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10.0)
    
    if not device:
        print(f"Could not find {DEVICE_NAME}. Is it powered on and advertising?")
        return

    print(f"Found {DEVICE_NAME} at {device.address}. Connecting...")
    
    async with BleakClient(device) as client:
        print("Connected! Subscribing to live data stream...")
        
        # Start receiving data
        await client.start_notify(CHARACTERISTIC_UUID, notification_handler)
        
        # Keep the program running for EXACTLY 10 minutes (600 seconds)
        collection_time_seconds = 600
        print(f"Collecting data for {collection_time_seconds / 60} minutes. Do not close terminal...")
        
        await asyncio.sleep(collection_time_seconds)
        
        # Time is up! Stop receiving.
        print("10 Minutes completed. Stopping data stream...")
        await client.stop_notify(CHARACTERISTIC_UUID)
        
    # --- Save to Excel ---
    print(f"Total readings collected: {len(collected_data)}")
    print("Saving to Excel file...")
    
    # Define columns based on what the ESP32 sent
    columns = ["Timestamp", "ax", "ay", "az", "gx", "gy", "gz", "Predicted_Class", "Confidence"]
    df = pd.DataFrame(collected_data, columns=columns)
    
    # Save the dataframe to an Excel file
    filename = "har_10_min_session.xlsx"
    df.to_excel(filename, index=False)
    print(f"Success! Data saved to {filename}")

if __name__ == "__main__":
    asyncio.run(main())