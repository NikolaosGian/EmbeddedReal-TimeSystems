from fileinput import filename
import matplotlib.pyplot as plt
import pandas as pd
import os

receivedTime = []
WritedTime = []
differentTime = []

symbols = ["APPL", "AMZN", "BINANCE:BTCUSDT", "IC MARKETS:1"]


for symbol in symbols:
    fileName = symbol + '/'''+ symbol+ "_times.txt"
    #print(fileName+'\n')
    file_exists = os.path.isfile(fileName)
    #print("fileExtist = " , file_exists)
    if(file_exists):
        #print("file Extists!")
        for line in open(fileName, "r"):
            lines = [int(i) for i in line.split()]
            #print("lines[0]", lines[0],"\n")
            #print("lines[1]", lines[1],"\n")
            
            receivedTime.append(int(lines[0]))
            WritedTime.append(int(lines[1]) *1000)
            dif = int(lines[0]) - int(lines[1])
            #print("dif = ",dif,"\n")
            if(dif < 0):
                differentTime.append(int(lines[1]) - int(lines[0]))
            elif (dif > 0):
                differentTime.append(dif)
            else:
            	differentTime.append(0)

        fig,ax = plt.subplots()
        ax.set_title(symbol)
        ax.plot(receivedTime,
            WritedTime,
            color="red", 
            marker="o")
        ax.set_xlabel("Received Time", fontsize = 14)
        ax.set_ylabel("Writed Time",
                color="red",
                fontsize=14)

        # twin object for two different y-axis on the sample plot
        ax2=ax.twinx()
        # make a plot with different y-axis using second axis object
        ax2.plot(receivedTime,differentTime,color="blue",marker="o")
        ax2.set_ylabel("Different Time",color="blue",fontsize=14)
        #plt.show()
        # save the plot as a file
        #name = symbol +'.jpg'
        #print(name+'\n')
        
        fig.savefig(symbol+'.jpg',
                    format='jpeg',
                    dpi=100,
                    bbox_inches='tight')
    else:
        continue
    
        
