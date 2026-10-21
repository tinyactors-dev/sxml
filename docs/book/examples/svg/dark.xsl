<?xml version="1.0" encoding="UTF-8"?>
<!-- A dark theme for an SVG drawing: every colour of the light palette,
     in the style text and in colour attributes, becomes its dark
     counterpart, the drawing gets a dark background, and what has no
     colour of its own becomes light. Everything else is copied as it is. -->
<xsl:stylesheet version="4.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:map="http://www.w3.org/2005/xpath-functions/map"
    xmlns:svg="http://www.w3.org/2000/svg"
    xmlns:theme="urn:example:theme"
    exclude-result-prefixes="#all">

  <xsl:mode on-no-match="shallow-copy"/>
  <xsl:output omit-xml-declaration="yes"/>

  <xsl:variable name="palette" as="map(xs:string, xs:string)" select="{
    '#eef3fb': '#1f2a3d',
    '#3b5b92': '#8fb0e8',
    '#f6efe3': '#3a3020',
    '#8a6a2f': '#d9b878',
    '#1b1b1b': '#e8e8e8',
    '#555555': '#b0b0b0',
    '#ffffff': '#121212'
  }"/>

  <!-- The text with every colour of the palette replaced. -->
  <xsl:function name="theme:dark" as="xs:string">
    <xsl:param name="text" as="xs:string"/>
    <xsl:sequence select="fold-left(map:keys($palette), $text,
      fn($result, $colour) { replace($result, $colour, $palette($colour), 'qi') })"/>
  </xsl:function>

  <xsl:template match="/svg:svg">
    <xsl:copy>
      <xsl:apply-templates select="@*"/>
      <xsl:attribute name="style"
          select="string-join((@style, 'background-color: #121212'), '; ')"/>
      <!-- What has no colour of its own is black; make it light. -->
      <xsl:if test="not(@fill)">
        <xsl:attribute name="fill">#e8e8e8</xsl:attribute>
      </xsl:if>
      <xsl:apply-templates/>
    </xsl:copy>
  </xsl:template>

  <xsl:template match="svg:style/text()">
    <xsl:value-of select="theme:dark(.)"/>
  </xsl:template>

  <xsl:template match="@fill | @stroke | @stop-color">
    <xsl:attribute name="{name()}" select="theme:dark(.)"/>
  </xsl:template>

</xsl:stylesheet>
