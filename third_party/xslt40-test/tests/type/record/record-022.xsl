<?xml version="1.0" encoding="utf-8"?> 

<xsl:stylesheet
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="4.0"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:cx="http://saxon.sf.net/complex"
    exclude-result-prefixes="xs cx"
    expand-text="yes"
    >
  
   <!-- Test priorities for record types -->
    
    <xsl:template match="~record(first as xs:string, last as xs:string, middle)" priority="10">
      <a><xsl:next-match/></a>
    </xsl:template>
  
  <xsl:template match="~record(first as xs:string, last as xs:string, middle as xs:string)" priority="11">
    <b><xsl:next-match/></b>
  </xsl:template>
  
  <xsl:template match="~record(first as xs:NCName, last as xs:NCName, middle as xs:string)" priority="12">
    <c><xsl:next-match/></c>
  </xsl:template>
    
    <xsl:template name="xsl:initial-template">
      <xsl:variable name="in" select="map{'first': xs:NCName('John'),
                                          'middle': xs:NCName('Henry'),
                                          'last': xs:NCName('Newman')}"
                    as="record(first as xs:NCName, middle as xs:NCName, last as xs:NCName)"/>
        <out><xsl:apply-templates select="$in"/></out>
    </xsl:template>  
    
</xsl:stylesheet>
