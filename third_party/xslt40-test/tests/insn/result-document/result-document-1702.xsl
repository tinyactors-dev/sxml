<?xml version="1.0" encoding="UTF-8"?>
<xsl:transform xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
	xmlns:xs="http://www.w3.org/2001/XMLSchema" version="4.0">
	
	
	
	<xsl:template name="xsl:initial-template">
		<xsl:result-document method="csv" csv-header="yes">
			<xsl:select>
			parse-csv("a,b,c
d,e,f
g,h,i", {'header':true()})
			</xsl:select>
		</xsl:result-document>
	</xsl:template>
</xsl:transform>